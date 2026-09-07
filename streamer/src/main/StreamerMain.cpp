#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "audio/AudioPacket.h"
#include "sources/MemoryAudioSource.h"
#include "sources/SourceFactory.h"
#include "sources/StdinPcmSource.h"

#ifdef NEXUS_STREAMER_REAL_NET
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <thread>

#include "app/StreamerApp.h"
#include "control/TcpLineTransport.h"
#include "identity/ApCredentials.h"
#include "identity/StreamerIdentity.h"
#include "send/UdpPacketSink.h"
#include "web/HttplibStreamerTransport.h"
#ifdef NEXUS_STREAMER_AVAHI
#include "discovery/AvahiStreamerDiscovery.h"
#endif
#endif

// Entry point. Argument parsing only — the process itself is StreamerApp, which runs the web UI,
// the control channel, and the audio engine as concurrent services.
//
// This replaces the old serve()/streamStdin() split, where the two were mutually exclusive branches
// and a process serving the UI could never send audio (so the play button was inert).

namespace {

constexpr int kRate = nexus::audio::wire::kSampleRate;
constexpr int kCh = nexus::audio::wire::kChannels;
constexpr std::uint32_t kBlock = 480;  // 10 ms @ 48k

// Future-stamp lead in seconds. Must exceed the speaker's StreamSync target (~80 ms) plus its
// 10-packet prefill and Wi-Fi jitter, and must also cover clock skew between this host and the
// speakers. Overridable with --lead.
double g_lead = 0.18;

// A `seconds`-long 440 Hz stereo sine at modest amplitude.
std::vector<std::int16_t> makeTone(double seconds) {
  const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
  std::vector<std::int16_t> pcm(frames * kCh);
  for (std::size_t f = 0; f < frames; ++f) {
    const double s = std::sin(2.0 * M_PI * 440.0 * f / kRate) * 8000.0;
    pcm[f * kCh] = pcm[f * kCh + 1] = static_cast<std::int16_t>(s);
  }
  return pcm;
}

#ifdef NEXUS_STREAMER_REAL_NET
nexus::streamer::app::StreamerApp* g_app = nullptr;
std::atomic<bool> g_signalled{false};

void onSignal(int) {
  g_signalled = true;
  if (g_app) g_app->requestShutdown();
}

// Split "10.0.0.5,10.0.0.6" into endpoints on the audio port.
std::vector<nexus::streamer::audio::Endpoint> parseTargets(const std::string& csv) {
  std::vector<nexus::streamer::audio::Endpoint> out;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::string ip =
        csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!ip.empty()) out.push_back({ip, nexus::audio::wire::kDefaultPort});
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string identityKeyPath() {
  if (const char* env = std::getenv("NEXUS_STREAMER_KEY"); env && *env) return env;
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.nexus-streamer/identity.key";
  }
  return ".nexus-streamer-identity.key";
}

std::string configPath() {
  if (const char* env = std::getenv("NEXUS_STREAMER_CONFIG"); env && *env) return env;
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.nexus-streamer/config.json";
  }
  return ".nexus-streamer-config.json";
}
#endif

}  // namespace

int main(int argc, char** argv) {
  std::string tone_ip;
  std::string stream_ips;  // comma-separated for multi-room fan-out
  std::string play_file;
  double seconds = 2.0;
  bool do_serve = false;
  int serve_port = 8090;
  std::string bind_address = "127.0.0.1";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version") {
      std::cout << "nexus-streamer 0.2.0\n";
      return 0;
    }
    if (arg == "--help") {
      std::cout
          << "Usage: " << argv[0]
          << " [--serve [port]] [--stream <ip[,ip2,...]>] [--tone <ip>] [--seconds N] [--lead S]\n"
          << "  --serve [port]       Run the browser control UI (default port 8090)\n"
          << "  --stream <ip[,...]>  Stream raw s16le/48k/stereo PCM from stdin to speaker(s)\n"
          << "                       e.g. ffmpeg -f avfoundation -i \":BlackHole 2ch\" \\\n"
          << "                            -ar 48000 -ac 2 -f s16le - | " << argv[0]
          << " --stream 10.0.0.5\n"
          << "  --tone <ip>          Send a 440 Hz test tone to <ip>:50005\n"
          << "  --play <file>        Play an audio file (WAV natively; others via ffmpeg)\n"
          << "  --seconds N          Tone duration (default 2)\n"
          << "  --lead S             Future-stamp lead in seconds (default 0.18)\n"
          << "  --bind <addr>        Web UI bind address (default 127.0.0.1; use 0.0.0.0 for LAN)\n"
          << "\n--serve combines with --stream/--tone/--play: UI and audio run together.\n"
          << "The control API requires a bearer token, printed at startup.\n";
      return 0;
    }
    if (arg == "--serve") {
      do_serve = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') serve_port = std::stoi(argv[++i]);
    }
    if (arg == "--stream" && i + 1 < argc) stream_ips = argv[++i];
    if (arg == "--lead" && i + 1 < argc) g_lead = std::stod(argv[++i]);
    if (arg == "--tone" && i + 1 < argc) tone_ip = argv[++i];
    if (arg == "--play" && i + 1 < argc) play_file = argv[++i];
    if (arg == "--bind" && i + 1 < argc) bind_address = argv[++i];
    if (arg == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
  }

#ifndef NEXUS_STREAMER_REAL_NET
  (void)do_serve;
  (void)serve_port;
  (void)seconds;
  (void)tone_ip;
  (void)stream_ips;
  std::cerr << "nexus-streamer: built without real sockets (NEXUS_STREAMER_REAL_NET=OFF)\n";
  return 1;
#else
  // `--ap-credentials`: print the derived private-AP SSID + passphrase for scripts/streamer-ap.sh
  // to bring up the permanent Nexus-Audio AP. Keeps the KDF in exactly one place (the C++ module).
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--ap-credentials") {
      nexus::streamer::identity::StreamerIdentity id(identityKeyPath());
      if (const auto st = id.load(); !st.ok()) {
        std::cerr << "nexus-streamer: identity load failed: " << st.message() << "\n";
        return 1;
      }
      const auto creds = nexus::identity::deriveApCredentials(id.streamerId());
      std::cout << "NEXUS_AP_SSID=" << creds.ssid << "\n"
                << "NEXUS_AP_PASSPHRASE=" << creds.passphrase << "\n";
      return 0;
    }
  }

  if (!do_serve && stream_ips.empty() && tone_ip.empty() && play_file.empty()) {
    std::cerr << "nexus-streamer: nothing to do (try --serve, --stream <ip>, --tone <ip>, "
                 "or --play <file>)\n";
    return 0;
  }

  // Persistent streamer identity: generated once and reused, so a paired speaker keeps accepting
  // this streamer's signed commands across restarts.
  const std::string key_path = identityKeyPath();
  nexus::streamer::identity::StreamerIdentity id(key_path);
  if (auto s = id.load(); !s.ok()) {
    std::cerr << "nexus-streamer: identity load failed: " << s.message() << "\n";
    return 1;
  }
  std::cout << "nexus-streamer: identity " << id.streamerId() << " (" << key_path << ")\n"
            << "  public key (pair a speaker to it):\n  " << id.publicKeyBase64() << "\n";

  nexus::streamer::control::TcpLineTransport line;
  nexus::streamer::send::UdpPacketSink sink;
  if (!sink.open().ok()) {
    std::cerr << "nexus-streamer: failed to open UDP socket\n";
    return 1;
  }
  nexus::streamer::web::HttplibStreamerTransport web;

  nexus::streamer::app::StreamerApp::Options options;
  options.web_port = serve_port;
  options.lead_seconds = g_lead;
  options.sample_rate = kRate;
  options.channels = kCh;
  options.block_frames = kBlock;
  options.config_path = configPath();
  options.bind_address = bind_address;
#ifdef NEXUS_STREAMER_AVAHI
  options.discovery = std::make_shared<nexus::streamer::discovery::AvahiStreamerDiscovery>();
#endif
  web.setBindAddress(bind_address);

  nexus::streamer::app::StreamerApp app(id, line, sink, web, options);
  if (auto st = app.startup(); !st.ok()) {
    std::cerr << "nexus-streamer: startup failed: " << st.message() << "\n";
    return 1;
  }
  g_app = &app;
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  if (do_serve) {
    std::cout << "nexus-streamer: control UI on http://" << bind_address << ":" << serve_port
              << "\n  API token: " << app.authToken() << "\n";
    if (bind_address == "127.0.0.1") {
      std::cout << "  (loopback only; use --bind 0.0.0.0 to reach it from the LAN)\n";
    }
  }

  // An audio source given on the command line starts immediately and keeps running alongside the
  // UI — the two are no longer alternatives.
  if (!stream_ips.empty()) {
    auto targets = parseTargets(stream_ips);
    auto src = std::make_unique<nexus::streamer::sources::StdinPcmSource>(stdin, kCh);
    if (auto st = app.playTo(targets, std::move(src)); !st.ok()) {
      std::cerr << "nexus-streamer: " << st.message() << "\n";
      app.shutdown();
      return 1;
    }
    std::cerr << "nexus-streamer: streaming stdin to " << targets.size() << " speaker(s) on udp/"
              << nexus::audio::wire::kDefaultPort << "\n";
  } else if (!play_file.empty()) {
    // Play a file directly — no external ffmpeg pipe needed for WAV.
    auto src = nexus::streamer::sources::makeSource({"file", play_file});
    if (!src.ok()) {
      std::cerr << "nexus-streamer: " << src.status().message() << "\n";
      app.shutdown();
      return 1;
    }
    std::vector<nexus::streamer::audio::Endpoint> targets;
    for (const auto& e : parseTargets(stream_ips.empty() ? tone_ip : stream_ips)) targets.push_back(e);
    if (targets.empty()) {
      std::cerr << "nexus-streamer: --play needs a target (--stream <ip> or --tone <ip>)\n";
      app.shutdown();
      return 1;
    }
    if (auto st = app.playTo(targets, std::move(src.value())); !st.ok()) {
      std::cerr << "nexus-streamer: " << st.message() << "\n";
      app.shutdown();
      return 1;
    }
    std::cerr << "nexus-streamer: playing " << play_file << " to " << targets.size()
              << " speaker(s)\n";
  } else if (!tone_ip.empty()) {
    auto src = std::make_unique<nexus::streamer::sources::MemoryAudioSource>(makeTone(seconds), kCh);
    if (auto st = app.playTo({{tone_ip, nexus::audio::wire::kDefaultPort}}, std::move(src));
        !st.ok()) {
      std::cerr << "nexus-streamer: " << st.message() << "\n";
      app.shutdown();
      return 1;
    }
    std::cerr << "nexus-streamer: sending a " << seconds << "s tone to " << tone_ip << "\n";
  }

  // A one-shot send (tone, or a finite stdin stream) with no UI exits when the audio ends; --serve
  // keeps running until Ctrl-C.
  if (!do_serve) {
    using nexus::streamer::audio::AudioEngine;
    while (!g_signalled.load() && app.audio().transport().state != AudioEngine::State::Idle) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cerr << "nexus-streamer: sent " << app.audio().transport().packets_sent << " packets\n";
    app.shutdown();
    return 0;
  }

  const int rc = app.run();
  g_app = nullptr;
  std::cout << "nexus-streamer: stopped\n";
  return rc;
#endif
}
