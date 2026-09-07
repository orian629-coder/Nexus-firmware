// Cross-module integration: Config + Identity + SecureStorage + SystemManager wired over the
// EventBus, exercising the paths that matter for boot and fault handling.

#include <gtest/gtest.h>
#include <sodium.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "discovery/DiscoveryService.h"
#include "identity/Crypto.h"
#include "identity/DeviceIdentity.h"
#include "identity/KeyManager.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioReceiver.h"
#include "audio/PlaybackManager.h"
#include "audio/StreamSync.h"
#include "audio/IAudioOutputHal.h"
#include "amplifier/AmplifierManager.h"
#include "calibration/CalibrationManager.h"
#include "calibration/CalibrationProfiles.h"
#include "diagnostics/DiagnosticService.h"
#include "dsp/DspEngine.h"
#include "microphone/MicrophoneManager.h"
#include "control/CommandServer.h"
#include "control/ICommandTransport.h"
#include "system/Watchdog.h"
#include "updater/UpdateManager.h"
#include "web/WebServer.h"
#include "web/IWebTransport.h"
#include "network/NetworkManager.h"
#include "identity/ApCredentials.h"
#include "network/INetworkHal.h"
#include "network/StreamerApJoin.h"
#include "pairing/PairingService.h"
#include "status/StatusService.h"
#include "storage/SecureStorage.h"
#include "system/SystemManager.h"

namespace fs = std::filesystem;
using namespace nexus;

namespace {
fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_it_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
}  // namespace

TEST(Integration, BootWiresConfigIdentityAndState) {
  auto dir = sandbox("boot");
  core::EventBus bus;

  storage::SecureStorage secrets((dir / "secure").string());
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  system::SystemManager sys(&bus);

  ASSERT_TRUE(sys.start().ok());
  ASSERT_TRUE(cfg.start().ok());
  ASSERT_TRUE(id.start().ok());

  // Fresh device: config unpaired → Unconfigured after initial-state resolution.
  sys.enterInitialState(cfg.get().pairing.paired);
  EXPECT_EQ(sys.states().current(), system::SystemState::Unconfigured);
  EXPECT_FALSE(id.deviceId().empty());

  sys.stop();
}

TEST(Integration, ConfigInvalidEventDrivesDegraded) {
  auto dir = sandbox("degraded");
  core::EventBus bus;
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  system::SystemManager sys(&bus);
  ASSERT_TRUE(sys.start().ok());
  ASSERT_TRUE(cfg.start().ok());

  // Reach Online via the normal path.
  sys.enterInitialState(true);
  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});
  bus.publish(core::Event{core::EventType::StreamerFound, "d"});
  bus.publish(core::Event{core::EventType::PairingCompleted, "p"});
  bus.drain();
  ASSERT_EQ(sys.states().current(), system::SystemState::Online);

  // A corrupt config on reload should raise ConfigInvalid and move the system to Degraded.
  { std::ofstream(dir / "config.json") << "{ not json"; }
  cfg.reloadFromDisk();
  bus.drain();
  EXPECT_EQ(sys.states().current(), system::SystemState::Degraded);

  sys.stop();
}

TEST(Integration, SignedIdentityUsableForCommandAuth) {
  auto dir = sandbox("sign");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  ASSERT_TRUE(id.start().ok());

  auto sig = id.sign(std::string("cmd-1:SET_VOLUME:70"));
  ASSERT_TRUE(sig.ok());
  EXPECT_FALSE(sig.value().empty());
}

// Full Phase-2 onboarding: a fresh, unpaired speaker pairs with a (fake) streamer, joins Wi-Fi,
// discovers the streamer, and reaches ONLINE — driven entirely through the EventBus + SystemManager.
TEST(Integration, Phase2OnboardingReachesOnline) {
  ASSERT_GE(sodium_init(), 0);
  auto dir = sandbox("onboard");
  core::EventBus bus;

  storage::SecureStorage secrets((dir / "secure").string());
  identity::KeyManager keys(&secrets);
  ASSERT_TRUE(keys.generateIfMissing().ok());
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());

  system::SystemManager sys(&bus);
  network::NetworkManager net(&bus);
  discovery::DiscoveryService disc(&bus);
  pairing::PairingService pair(&bus, &keys, &config, &secrets);
  ASSERT_TRUE(sys.start().ok());
  ASSERT_TRUE(net.start().ok());
  ASSERT_TRUE(disc.start().ok());
  ASSERT_TRUE(pair.start().ok());

  // Fresh device → Unconfigured.
  sys.enterInitialState(config.get().pairing.paired);
  ASSERT_EQ(sys.states().current(), system::SystemState::Unconfigured);
  ASSERT_TRUE(sys.states().transitionTo(system::SystemState::SetupMode, "user setup").ok());

  // Build a fake streamer whose id matches the one StubDiscoveryHal advertises ("STR-LAB01").
  std::vector<std::uint8_t> spk(crypto_sign_PUBLICKEYBYTES), ssk(crypto_sign_SECRETKEYBYTES);
  crypto_sign_keypair(spk.data(), ssk.data());

  auto boxpub = keys.boxPublicKeyBase64();
  ASSERT_TRUE(boxpub.ok());
  auto boxpubbin = identity::crypto::fromBase64(boxpub.value());

  pairing::PairingRequest req;
  req.streamer_id = "STR-LAB01";
  req.streamer_public_key = identity::crypto::toBase64(spk);
  req.site_id = "site-1";
  req.initial_speaker_name = "Kitchen";
  req.setup_code = "424242";
  nlohmann::json wj = {{"ssid", "NexusLab"}, {"psk", "wifi-pass"}};
  std::string ws = wj.dump();
  auto sealed = identity::crypto::sealTo(boxpubbin.value(),
                                         std::vector<std::uint8_t>(ws.begin(), ws.end()));
  req.sealed_wifi_b64 = identity::crypto::toBase64(sealed.value());
  std::string canon = req.canonicalString();
  std::vector<std::uint8_t> sig(crypto_sign_BYTES);
  crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(canon.data()),
                       canon.size(), ssk.data());
  req.signature_b64 = identity::crypto::toBase64(sig);

  // Pair. This persists pairing (paired=true) and yields Wi-Fi credentials.
  ASSERT_TRUE(pair.beginSetupMode("424242", 1000, 300).ok());
  auto pres = pair.processRequest(req, 1100);
  ASSERT_TRUE(pres.ok());
  bus.drain();
  EXPECT_TRUE(config.get().pairing.paired);

  // Move to CONNECTING_NETWORK, then join Wi-Fi with the paired credentials → NetworkConnected →
  // SystemManager transitions to SEARCHING_STREAMER.
  ASSERT_TRUE(sys.states().transitionTo(system::SystemState::ConnectingNetwork, "paired").ok());
  ASSERT_TRUE(net.connectWifi(pres.value().wifi.ssid, pres.value().wifi.psk).ok());
  bus.drain();
  EXPECT_EQ(sys.states().current(), system::SystemState::SearchingStreamer);

  // Find the streamer → StreamerFound → AUTHENTICATING.
  auto found = disc.findStreamer(config.get().pairing.streamer_id);
  bus.drain();
  ASSERT_TRUE(found.ok());
  EXPECT_EQ(sys.states().current(), system::SystemState::Authenticating);

  // Authentication success (Phase 3 will do the real signed handshake) → ONLINE.
  bus.publish(core::Event{core::EventType::PairingCompleted, "pairing"});
  bus.drain();
  EXPECT_EQ(sys.states().current(), system::SystemState::Online);

  pair.stop();
  disc.stop();
  net.stop();
  sys.stop();
}

// Phase 1 wiring: a paired speaker that reaches SEARCHING_STREAMER must discover its streamer
// AUTOMATICALLY — no manual findStreamer() call — and advance to AUTHENTICATING. Before this
// wiring, DiscoveryService::findStreamer() had no caller and a paired device sat in
// SEARCHING_STREAMER forever. This test installs the same StateChanged→startSearching subscription
// that Application.cpp wires, and asserts the auto-advance. The stub HAL advertises "STR-LAB01".
TEST(Integration, PairedSpeakerAutoDiscoversStreamerFromStateWiring) {
  auto dir = sandbox("autodisc");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());
  ASSERT_TRUE(config
                  .update([](config::SpeakerConfig& c) {
                    c.pairing.paired = true;
                    c.pairing.streamer_id = "STR-LAB01";  // matches StubDiscoveryHal
                    c.pairing.streamer_public_key = "c3RyZWFtZXItcHVibGljLWtleQ==";
                  })
                  .ok());

  system::SystemManager sys(&bus);
  discovery::DiscoveryService disc(&bus);
  ASSERT_TRUE(sys.start().ok());
  ASSERT_TRUE(disc.start().ok());

  // The wiring under test (mirrors Application::buildServices): entering SEARCHING_STREAMER starts
  // the browse worker for the paired streamer; leaving it stops the worker. A short interval keeps
  // the test snappy (the first browse is immediate regardless).
  bus.subscribe(core::EventType::StateChanged, [&](const core::Event& e) {
    const std::string to = e.data.value("to", "");
    const std::string from = e.data.value("from", "");
    if (to == system::toString(system::SystemState::SearchingStreamer)) {
      const auto& p = config.get().pairing;
      disc.startSearching(p.streamer_id, p.streamer_public_key, std::chrono::milliseconds(10));
    } else if (from == system::toString(system::SystemState::SearchingStreamer)) {
      disc.stopSearching();
    }
  });

  // Paired device → CONNECTING_NETWORK; the uplink coming up drives SEARCHING_STREAMER, which must
  // auto-advance to AUTHENTICATING with no manual discovery call.
  sys.enterInitialState(config.get().pairing.paired);
  ASSERT_EQ(sys.states().current(), system::SystemState::ConnectingNetwork);
  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});

  bool reached = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (sys.states().current() == system::SystemState::Authenticating) {
      reached = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(reached) << "did not auto-discover; state="
                       << system::toString(sys.states().current());

  disc.stop();
  sys.stop();
}

// After pairing, the paired streamer's signed command flows through the CommandServer and the
// StatusService heartbeat reflects the resulting state — the Phase-3 control loop end to end.
TEST(Integration, Phase3SignedCommandAndHeartbeat) {
  ASSERT_GE(sodium_init(), 0);
  auto dir = sandbox("control");
  core::EventBus bus;

  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  ASSERT_TRUE(id.start().ok());
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());

  // Streamer keypair; pair the device to it.
  std::vector<std::uint8_t> spk(crypto_sign_PUBLICKEYBYTES), ssk(crypto_sign_SECRETKEYBYTES);
  crypto_sign_keypair(spk.data(), ssk.data());
  config.update([&](config::SpeakerConfig& c) {
    c.pairing.paired = true;
    c.pairing.streamer_id = "STR-1";
    c.pairing.streamer_public_key = identity::crypto::toBase64(spk);
  });

  std::int64_t now = 5000;
  auto txt = std::make_unique<control::StubCommandTransport>();
  auto* transport = txt.get();
  control::CommandServer server(&bus, &id, &config, [&] { return now; }, std::move(txt));
  ASSERT_TRUE(server.start().ok());

  status::StatusService status(&bus, &id, &config, [&] { return now; });

  // Streamer builds and signs a SET_VOLUME command.
  control::Command c;
  c.command_id = "cmd-1";
  c.target_id = id.deviceId();
  c.command = "SET_VOLUME";
  c.payload = {{"volume", 72}};
  c.timestamp = now;
  c.expires_at = now + 30;
  std::string canon = c.canonicalString();
  std::vector<std::uint8_t> sig(crypto_sign_BYTES);
  crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(canon.data()),
                       canon.size(), ssk.data());
  c.signature_b64 = identity::crypto::toBase64(sig);

  auto resp = nlohmann::json::parse(transport->deliver(c.toJson().dump()));
  ASSERT_TRUE(resp["ok"].get<bool>());
  EXPECT_EQ(config.get().audio.volume, 72);

  // The heartbeat reflects the new volume.
  auto hb = status.buildHeartbeat();
  EXPECT_EQ(hb["volume"].get<int>(), 72);

  server.stop();
}

// A stream of audio packets flows Streamer -> receiver -> jitter buffer -> playback -> output,
// synchronized by StreamSync, honoring the config volume.
TEST(Integration, Phase4AudioStreamPlaysThroughPipeline) {
  auto dir = sandbox("audio");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());
  config.update([](config::SpeakerConfig& c) {
    c.audio.muted = false;
    c.audio.volume = 100;
  });

  audio::AudioBuffer buffer(/*prefill*/ 3, /*capacity*/ 50);
  audio::StreamSync sync;
  double now = 1000.0;

  auto src = std::make_unique<audio::StubAudioSource>();
  auto* src_ptr = src.get();
  audio::AudioReceiver rx(&bus, &buffer, &sync, [&] { return now; }, std::move(src));
  ASSERT_TRUE(rx.start().ok());

  int started = 0;
  bus.subscribe(core::EventType::AudioStarted, [&](const core::Event&) { ++started; });

  // Streamer sends 5 packets of 480 stereo frames each.
  for (std::uint64_t seq = 1; seq <= 5; ++seq) {
    audio::AudioPacket p;
    p.header.sequence = seq;
    p.header.timestamp = now + 0.01 * static_cast<double>(seq);
    p.header.frame_count = 480;
    p.samples.assign(480 * 2, static_cast<std::int16_t>(2000));
    src_ptr->inject(audio::packPacket(p));
  }
  bus.drain();
  EXPECT_EQ(started, 1);
  EXPECT_EQ(buffer.depth(), 5u);

  // Play them out through a stub output device.
  audio::StubAudioOutputHal out;
  audio::PlaybackManager pb(&buffer, &sync, &out, &config, [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());
  std::size_t total = 0;
  for (int i = 0; i < 5; ++i) {
    now += 0.01;
    total += pb.step();
  }
  EXPECT_GT(total, 0u);
  EXPECT_EQ(pb.framesPlayed(), total);
  EXPECT_EQ(buffer.metrics().lost, 0u);

  rx.stop();
}

// Output HAL that captures written PCM so a test can measure the DSP effect.
namespace {
class CapturingOutputHal : public audio::IAudioOutputHal {
 public:
  core::Status open(int, int) override { return core::Status::success(); }
  core::Status close() override { return core::Status::success(); }
  core::Result<std::size_t> write(const std::int16_t* pcm, std::size_t frames) override {
    for (std::size_t i = 0; i < frames * 2; ++i) captured.push_back(pcm[i]);
    return frames;
  }
  std::vector<std::int16_t> captured;
};
double pcmRms(const std::vector<std::int16_t>& v) {
  double s = 0;
  for (auto x : v) s += double(x) * x;
  return v.empty() ? 0 : std::sqrt(s / static_cast<double>(v.size()));
}
}  // namespace

// The DSP engine, wired as PlaybackManager's hook, audibly shapes the stream: boosting the EQ
// band at the tone's frequency raises the output energy vs a flat chain.
TEST(Integration, Phase5DspHookShapesPlayback) {
  auto dir = sandbox("dsp");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());
  config.update([](config::SpeakerConfig& c) {
    c.audio.muted = false;
    c.audio.volume = 100;
  });

  auto makeTone = [](audio::AudioBuffer& buf, int packets) {
    for (std::uint64_t seq = 1; seq <= static_cast<std::uint64_t>(packets); ++seq) {
      audio::AudioPacket p;
      p.header.sequence = seq;
      p.header.frame_count = 480;
      p.samples.resize(480 * 2);
      for (std::size_t f = 0; f < 480; ++f) {
        auto s = static_cast<std::int16_t>(
            4000 * std::sin(2.0 * M_PI * 1000.0 *
                            (static_cast<double>((seq - 1) * 480 + f)) / 48000.0));
        p.samples[f * 2] = s;
        p.samples[f * 2 + 1] = s;
      }
      buf.push(p);
    }
  };

  // Flat DSP run.
  audio::AudioBuffer buf_flat(2, 50);
  audio::StreamSync sync_flat;
  makeTone(buf_flat, 6);
  CapturingOutputHal out_flat;
  dsp::DspEngine dsp_flat(&bus, 48000.0, 2);
  dsp_flat.setLimiter(false, -1.0);
  audio::PlaybackManager pb_flat(&buf_flat, &sync_flat, &out_flat, &config, [] { return 0.0; });
  pb_flat.setDspHook([&](std::int16_t* io, std::size_t fr, int ch) {
    dsp_flat.processInt16(io, fr, ch);
  });
  pb_flat.open(48000, 2);
  for (int i = 0; i < 6; ++i) pb_flat.step();

  // Boosted DSP run: +10 dB at the 1 kHz band (index 15).
  audio::AudioBuffer buf_boost(2, 50);
  audio::StreamSync sync_boost;
  makeTone(buf_boost, 6);
  CapturingOutputHal out_boost;
  dsp::DspEngine dsp_boost(&bus, 48000.0, 2);
  dsp_boost.setLimiter(false, -1.0);
  std::array<double, dsp::kEqBands> gains{};
  gains[15] = 10.0;  // kBandFreqs[15] == 1000 Hz
  dsp_boost.setEqGains(gains);
  audio::PlaybackManager pb_boost(&buf_boost, &sync_boost, &out_boost, &config, [] { return 0.0; });
  pb_boost.setDspHook([&](std::int16_t* io, std::size_t fr, int ch) {
    dsp_boost.processInt16(io, fr, ch);
  });
  pb_boost.open(48000, 2);
  for (int i = 0; i < 6; ++i) pb_boost.step();

  ASSERT_FALSE(out_flat.captured.empty());
  ASSERT_FALSE(out_boost.captured.empty());
  EXPECT_GT(pcmRms(out_boost.captured), pcmRms(out_flat.captured) * 1.2);
}

// An amplifier overheat propagates through the bus to StatusService as an immediate alert, and the
// DiagnosticService aggregates the amplifier + mic health into a single report.
TEST(Integration, Phase6HealthMonitoringAndDiagnostics) {
  auto dir = sandbox("health");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  id.start();
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();

  // Amplifier with a stub HAL we can heat up.
  auto amp_hal = std::make_unique<nexus::amplifier::StubAmplifierHal>();
  auto* amp_hal_ptr = amp_hal.get();
  nexus::amplifier::AmplifierManager amp(&bus, std::move(amp_hal), 70.0, 85.0);
  amp.start();

  nexus::microphone::MicrophoneManager mic(&bus);
  mic.start();

  // StatusService forwards critical events as immediate alerts.
  std::vector<nlohmann::json> alerts;
  std::mutex m;
  status::StatusService status(
      &bus, &id, &config, [] { return 1; },
      [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lk(m);
        alerts.push_back(j);
      },
      3600);
  ASSERT_TRUE(status.start().ok());

  // Overheat the amp; poll drives the state machine and emits AmplifierOverheat.
  amp_hal_ptr->temperature = 95.0;
  amp.poll();
  bus.drain();
  status.stop();

  bool overheat_alert = false;
  {
    std::lock_guard<std::mutex> lk(m);
    for (auto& a : alerts)
      if (a.value("alert", "") == "AMPLIFIER_OVERHEAT") overheat_alert = true;
  }
  EXPECT_TRUE(overheat_alert);
  EXPECT_EQ(amp.ampState(), nexus::amplifier::AmpState::Overheated);

  // Diagnostics aggregates: amp is overheated (warning/error), mic self-test ok.
  using nexus::diagnostics::CheckResult;
  nexus::diagnostics::DiagnosticService diag(&bus);
  diag.addCheck("amplifier", [&]() -> std::pair<CheckResult, std::string> {
    return amp.ampState() == nexus::amplifier::AmpState::Overheated
               ? std::make_pair(CheckResult::Warning, "overheated")
               : std::make_pair(CheckResult::Ok, std::string());
  });
  diag.addCheck("microphone", [&]() -> std::pair<CheckResult, std::string> {
    return mic.selfTest().ok() ? std::make_pair(CheckResult::Ok, std::string())
                               : std::make_pair(CheckResult::Error, "mic");
  });
  auto report = diag.run("diag-health");
  EXPECT_EQ(report.overall, CheckResult::Warning);
  auto j = report.toJson();
  EXPECT_EQ(j["checks"]["amplifier"], "warning");
  EXPECT_EQ(j["checks"]["microphone"], "ok");

  amp.stop();
  mic.stop();
}

// Full calibration wired to the real DSP + mic + config: capture a tone, analyze, apply the
// correction to the DSP engine, persist the profile, and record it in config — mirroring how
// Application wires calibration.
TEST(Integration, Phase7CalibrationAppliesToDspAndPersists) {
  auto dir = sandbox("calibrate");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();
  nexus::microphone::MicrophoneManager mic(&bus);
  mic.start();
  dsp::DspEngine dspEngine(&bus, 48000.0, 2);
  auto profiles = std::make_shared<nexus::calibration::CalibrationProfiles>((dir / "cal").string());

  nexus::calibration::CalibrationManager cal(&bus, &config, profiles, 48000.0);
  cal.start();

  bool eq_applied = false;
  cal.setHooks(
      [&](std::size_t frames) { return mic.capture(frames).value_or(std::vector<std::int16_t>{}); },
      [&](const std::array<double, dsp::kEqBands>& g) {
        dspEngine.setEqGains(g);
        eq_applied = true;
      },
      [](bool) {});

  auto res = cal.runCalibration("room");
  bus.drain();
  ASSERT_TRUE(res.ok());
  EXPECT_TRUE(eq_applied);
  EXPECT_EQ(cal.calState(), nexus::calibration::CalState::Completed);
  EXPECT_EQ(config.get().audio.eq_profile, "room");
  EXPECT_TRUE(profiles->has("room"));

  // A fresh CalibrationManager can re-apply the persisted profile (survives reboot).
  bool reapplied = false;
  nexus::calibration::CalibrationManager cal2(&bus, &config, profiles, 48000.0);
  cal2.setHooks([](std::size_t) { return std::vector<std::int16_t>{}; },
                [&](const std::array<double, dsp::kEqBands>&) { reapplied = true; },
                [](bool) {});
  ASSERT_TRUE(cal2.applySavedProfile("room").ok());
  EXPECT_TRUE(reapplied);

  mic.stop();
}

// The web API, wired to a real config, reflects device state and applies authorized changes —
// mirroring how Application builds the ApiContext.
TEST(Integration, Phase8WebApiReflectsAndControlsConfig) {
  auto dir = sandbox("web");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();
  config.update([](config::SpeakerConfig& c) { c.audio.volume = 41; });

  nexus::web::ApiContext ctx;
  ctx.status = [&] {
    return nlohmann::json{{"volume", config.get().audio.volume},
                          {"muted", config.get().audio.muted}};
  };
  ctx.audio = [&] { return nlohmann::json{{"volume", config.get().audio.volume}}; };
  ctx.setVolume = [&](const nlohmann::json& b) -> std::pair<bool, std::string> {
    auto s = config.update([&](config::SpeakerConfig& c) { c.audio.volume = b["volume"].get<int>(); });
    return {s.ok(), "ok"};
  };

  nexus::web::WebServer web(&bus, ctx, "secret-token");
  ASSERT_TRUE(web.start().ok());
  auto* t = static_cast<nexus::web::StubWebTransport*>(web.transport());

  // GET reflects the current config.
  auto st = nlohmann::json::parse(t->handle({"GET", "/api/status", "", ""}).body);
  EXPECT_EQ(st["volume"].get<int>(), 41);

  // Unauthorized write rejected; config unchanged.
  auto bad = t->handle({"POST", "/api/audio/volume", "{\"volume\":77}", ""});
  EXPECT_EQ(bad.status, 401);
  EXPECT_EQ(config.get().audio.volume, 41);

  // Authorized write applied and visible on the next GET.
  auto good = t->handle({"POST", "/api/audio/volume", "{\"volume\":77}", "Bearer secret-token"});
  EXPECT_EQ(good.status, 200);
  EXPECT_EQ(config.get().audio.volume, 77);
  auto st2 = nlohmann::json::parse(t->handle({"GET", "/api/status", "", ""}).body);
  EXPECT_EQ(st2["volume"].get<int>(), 77);

  web.stop();
}

// Reliability: the watchdog escalates a persistently-unhealthy service to SystemManager, which
// degrades and, after repeated timeouts, enters Safe Mode (which disables normal operation).
TEST(Integration, Phase9WatchdogEscalatesToSafeMode) {
  core::EventBus bus;
  system::SystemManager sys(&bus);
  int disabled = 0;
  sys.setSafeModeDisableAction([&] { ++disabled; });
  ASSERT_TRUE(sys.start().ok());
  sys.enterInitialState(true);
  // Reach Online (watchdog failures happen during normal operation).
  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});
  bus.publish(core::Event{core::EventType::StreamerFound, "d"});
  bus.publish(core::Event{core::EventType::PairingCompleted, "p"});
  bus.drain();
  ASSERT_EQ(sys.states().current(), system::SystemState::Online);

  // A watchdog watching a service that is always unhealthy; recovery publishes WatchdogTimeout
  // (SystemManager consumes it). Threshold 1 so each poll fires a timeout.
  struct AlwaysSick : core::IService {
    std::string name() const override { return "audio"; }
    core::Status start() override { return {}; }
    core::Status stop() override { return {}; }
    core::ServiceState state() const override { return core::ServiceState::Running; }
    core::Status healthCheck() override { return core::Status::error(core::ErrorCode::Unknown, "x"); }
  } sick;

  system::Watchdog wd(&bus, 5000, 1);
  wd.watch(&sick);

  std::atomic<int> safe{0};
  bus.subscribe(core::EventType::SafeModeEntered, [&](const core::Event&) { ++safe; });

  // Three failing polls → three WatchdogTimeouts → SystemManager escalates to Safe Mode.
  wd.pollOnce();
  wd.pollOnce();
  wd.pollOnce();
  bus.drain();

  EXPECT_TRUE(sys.safeMode().active());
  EXPECT_EQ(disabled, 1);
  EXPECT_EQ(safe.load(), 1);
  EXPECT_EQ(sys.states().current(), system::SystemState::Degraded);
  sys.stop();
}

// Reliability: a signed OTA that fails its post-install health check rolls the binary back and
// the system remains recoverable.
TEST(Integration, Phase9FailedUpdateRollsBack) {
  ASSERT_GE(sodium_init(), 0);
  auto dir = sandbox("ota");
  auto target = (dir / "nexus-speaker").string();
  { std::ofstream(target) << "RUNNING_V1"; }

  // Vendor signs a v2 package.
  std::vector<std::uint8_t> vpk(crypto_sign_PUBLICKEYBYTES), vsk(crypto_sign_SECRETKEYBYTES);
  crypto_sign_keypair(vpk.data(), vsk.data());
  auto payload = std::vector<std::uint8_t>{'V', '2'};
  nexus::updater::UpdatePackage pkg;
  pkg.payload = payload;
  pkg.manifest.version = "2.0.0";
  pkg.manifest.min_hardware_version = "1.0";
  pkg.manifest.payload_sha256 = identity::crypto::sha256Hex(payload);
  std::string canon = pkg.manifest.canonicalString();
  std::vector<std::uint8_t> sig(crypto_sign_BYTES);
  crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(canon.data()),
                       canon.size(), vsk.data());
  pkg.signature_b64 = identity::crypto::toBase64(sig);

  auto src = std::make_unique<nexus::updater::StubUpdateSource>();
  src->package = pkg;

  core::EventBus bus;
  nexus::updater::UpdateManager upd(&bus, identity::crypto::toBase64(vpk), target, std::move(src));
  upd.setHealthCheck([] { return false; });  // new binary is unhealthy → rollback

  std::atomic<int> rolled{0};
  bus.subscribe(core::EventType::RollbackTriggered, [&](const core::Event&) { ++rolled; });

  EXPECT_FALSE(upd.runUpdate("http://vendor/pkg").ok());
  bus.drain();
  EXPECT_EQ(rolled.load(), 1);

  std::ifstream in(target);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "RUNNING_V1");  // rolled back to the working binary
}

namespace {
// A network HAL exposing a fixed scan list and recording the last connect — lets the wiring test
// prove a paired speaker joins its streamer AP when the state machine enters CONNECTING_NETWORK.
class ApJoinFakeHal : public network::INetworkHal {
 public:
  using INetworkHal::connectWifi;
  explicit ApJoinFakeHal(std::vector<network::WifiNetwork> scan) : scan_(std::move(scan)) {}
  core::Result<std::vector<network::WifiNetwork>> scanWifi() override { return scan_; }
  core::Status connectWifi(const network::WifiConnectParams& p) override {
    last_ssid = p.ssid;
    last_psk = p.psk;
    ++calls;
    connected_ = true;
    return core::Status::success();
  }
  core::Status disconnect() override {
    connected_ = false;
    return core::Status::success();
  }
  core::Result<network::NetworkStatus> status() override {
    network::NetworkStatus s;
    s.connected = connected_;
    s.mode = connected_ ? "wifi" : "";
    return s;
  }
  std::string last_ssid;
  std::string last_psk;
  int calls = 0;

 private:
  std::vector<network::WifiNetwork> scan_;
  bool connected_ = false;
};
}  // namespace

// Phase A wiring: on entering CONNECTING_NETWORK a paired speaker must auto-JOIN its streamer's
// private AP ("Nexus-<streamer_id>"). This installs the same StateChanged->joinStreamerAp
// subscription Application.cpp wires and asserts the join lands on the derived SSID/passphrase.
TEST(Integration, PairedSpeakerJoinsStreamerApOnConnectingNetwork) {
  auto dir = sandbox("apjoin");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());
  ASSERT_TRUE(config
                  .update([](config::SpeakerConfig& c) {
                    c.pairing.paired = true;
                    c.pairing.streamer_id = "STR-LAB01";
                  })
                  .ok());

  const auto creds = identity::deriveApCredentials("STR-LAB01");
  auto hal = std::make_unique<ApJoinFakeHal>(
      std::vector<network::WifiNetwork>{{creds.ssid, -40}, {"Handsome", -60}});
  auto* raw = hal.get();
  network::NetworkManager net(&bus, std::move(hal));

  system::SystemManager sys(&bus);
  ASSERT_TRUE(sys.start().ok());

  // The wiring under test (mirrors Application::buildServices).
  bus.subscribe(core::EventType::StateChanged, [&](const core::Event& e) {
    if (e.data.value("to", "") != system::toString(system::SystemState::ConnectingNetwork)) return;
    const auto& p = config.get().pairing;
    if (!p.paired || p.streamer_id.empty()) return;
    network::joinStreamerAp(net, p.streamer_id);
  });

  sys.enterInitialState(config.get().pairing.paired);  // -> CONNECTING_NETWORK, emits StateChanged
  bus.drain();

  EXPECT_EQ(raw->calls, 1);
  EXPECT_EQ(raw->last_ssid, creds.ssid);
  EXPECT_EQ(raw->last_psk, creds.passphrase);
}
