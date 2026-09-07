#include <gtest/gtest.h>

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/StreamerApp.h"
#include "audio/AudioPacket.h"
#include "config/ConfigManager.h"
#include "control/CommandServer.h"
#include "control/ICommandTransport.h"
#include "control/ILineTransport.h"
#include "core/EventBus.h"
#include "identity/Crypto.h"
#include "identity/DeviceIdentity.h"
#include "send/MemoryPacketSink.h"
#include "sources/MemoryAudioSource.h"
#include "storage/SecureStorage.h"
#include "web/IWebTransport.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_app_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// Forwards a line straight into the speaker's real CommandServer — no sockets, real signature
// verification and real config persistence. Records every command it carried so a test can assert
// what the speaker was actually told.
class LoopbackLineTransport : public streamer::control::ILineTransport {
 public:
  explicit LoopbackLineTransport(control::StubCommandTransport* t) : transport_(t) {}

  core::Result<std::string> request(const std::string&, int, const std::string& req) override {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      auto j = nlohmann::json::parse(req, nullptr, false);
      if (!j.is_discarded()) commands_.push_back(j.value("command", ""));
    }
    return transport_->deliver(req);
  }

  std::vector<std::string> commands() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return commands_;
  }

 private:
  control::StubCommandTransport* transport_;
  mutable std::mutex mutex_;
  std::vector<std::string> commands_;
};

// A real, paired speaker CommandServer running in-process.
struct SpeakerRig {
  fs::path dir;
  core::EventBus bus;
  std::unique_ptr<storage::SecureStorage> secrets;
  std::unique_ptr<identity::DeviceIdentity> id;
  std::unique_ptr<config::ConfigManager> config;
  std::unique_ptr<control::CommandServer> server;
  control::StubCommandTransport* transport = nullptr;
  std::int64_t now = 1000;

  SpeakerRig(const std::string& name, const std::string& streamer_pk_b64) : dir(sandbox(name)) {
    secrets = std::make_unique<storage::SecureStorage>((dir / "secure").string());
    id = std::make_unique<identity::DeviceIdentity>((dir / "identity/factory.json").string(),
                                                    secrets.get(), &bus);
    id->start();
    config = std::make_unique<config::ConfigManager>((dir / "config.json").string(), &bus);
    config->start();
    config->update([&](config::SpeakerConfig& c) {
      c.pairing.paired = true;
      c.pairing.streamer_id = "STR-1";
      c.pairing.streamer_public_key = streamer_pk_b64;
    });
    auto t = std::make_unique<control::StubCommandTransport>();
    transport = t.get();
    server = std::make_unique<control::CommandServer>(&bus, id.get(), config.get(),
                                                      [this] { return now; }, std::move(t));
    server->start();
  }
};

// A StreamerIdentity backed by a sandbox key file.
struct StreamerIdentityRig {
  fs::path dir;
  std::unique_ptr<streamer::identity::StreamerIdentity> id;

  explicit StreamerIdentityRig(const std::string& name) : dir(sandbox(name + "_id")) {
    id = std::make_unique<streamer::identity::StreamerIdentity>((dir / "identity.key").string());
    id->load();
  }
};

std::vector<std::int16_t> tone(std::size_t frames) {
  std::vector<std::int16_t> pcm(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    pcm[i * 2] = pcm[i * 2 + 1] = static_cast<std::int16_t>(i % 1000);
  }
  return pcm;
}

// The control API requires a bearer token; the app generates one at startup, so tests read it back
// from the app rather than hardcoding a value.
nexus::web::HttpRequest post(const std::string& path, const nlohmann::json& body,
                             const std::string& token = "") {
  nexus::web::HttpRequest r;
  r.method = "POST";
  r.path = path;
  r.body = body.dump();
  if (!token.empty()) r.auth = "Bearer " + token;
  return r;
}

}  // namespace

// ── The Phase-1 proof ────────────────────────────────────────────────────────────────────────────
//
// Before Phase 1, `--serve` and `--stream` were mutually exclusive main() branches: serve() blocked
// on the web server and never ran the send loop, so pressing play sent a START_AUDIO command to a
// speaker that then received no packets. This asserts BOTH halves now happen in one process from
// one request.
TEST(StreamerApp, TransportPlaySendsAudioAndCommandsTheSpeaker) {
  StreamerIdentityRig sid("play");
  SpeakerRig rig("play", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::send::MemoryPacketSink sink;
  nexus::web::StubWebTransport web;

  streamer::app::StreamerApp::Options opts;
  opts.enable_link_reporter = false;  // isolate the transport path from background REPORT_LINKs
  streamer::app::StreamerApp app(*sid.id, line, sink, web, opts);
  ASSERT_TRUE(app.startup().ok());

  app.registry().upsert({rig.id->deviceId(), "Living Room", "127.0.0.1", 45455});

  // Load a finite source, then press play through the HTTP surface exactly as the browser does.
  ASSERT_TRUE(app.playTo({{"127.0.0.1", nexus::audio::wire::kDefaultPort}},
                         std::make_unique<streamer::sources::MemoryAudioSource>(tone(4800), 2))
                  .ok());

  auto res = web.handle(post("/api/transport", {{"speaker", rig.id->deviceId()}, {"action", "play"}}, app.authToken()));
  EXPECT_EQ(res.status, 200) << res.body;

  // The speaker was told to start.
  const auto commands = line.commands();
  EXPECT_NE(std::find(commands.begin(), commands.end(), "START_AUDIO"), commands.end())
      << "the speaker never received START_AUDIO";

  // ...and real audio datagrams were emitted by the same process, in the same run.
  for (int i = 0; i < 100 && sink.count() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(sink.count(), 0u) << "no audio was sent — serve/stream are still not unified";

  // The datagrams are decodable by the speaker's own decoder (shared wire format).
  const auto recorded = sink.datagrams();
  nexus::audio::AudioPacket pkt;
  ASSERT_TRUE(nexus::audio::unpackPacket(recorded.front().data(), recorded.front().size(), 2, pkt));
  EXPECT_EQ(pkt.header.sequence, 0u);
  EXPECT_GT(pkt.header.timestamp, 0.0);

  app.shutdown();
}

// The control API can pair a speaker, which means it accepts a Wi-Fi PSK in plaintext, and
// /api/speakers exposes the whole site's topology. So every /api/ route requires the bearer token —
// GETs included — while the UI shell stays open so a browser can load and prompt for it.
TEST(StreamerApp, ApiRequiresBearerTokenButUiShellIsOpen) {
  StreamerIdentityRig sid("auth");
  SpeakerRig rig("auth", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::send::MemoryPacketSink sink;
  nexus::web::StubWebTransport web;

  streamer::app::StreamerApp::Options opts;
  opts.enable_link_reporter = false;
  opts.enable_monitor = false;
  streamer::app::StreamerApp app(*sid.id, line, sink, web, opts);
  ASSERT_TRUE(app.startup().ok());
  ASSERT_FALSE(app.authToken().empty()) << "no token was generated";

  // No token → refused.
  EXPECT_EQ(web.handle({"GET", "/api/speakers", "", ""}).status, 401);
  EXPECT_EQ(web.handle(post("/api/speakers", {{"device_id", "SPK-X"}, {"host", "10.0.0.9"}})).status,
            401);
  // Wrong token → refused.
  nexus::web::HttpRequest wrong{"GET", "/api/speakers", "", "Bearer not-the-token", ""};
  EXPECT_EQ(web.handle(wrong).status, 401);

  // Correct token → allowed.
  nexus::web::HttpRequest good{"GET", "/api/speakers", "", "Bearer " + app.authToken(), ""};
  EXPECT_EQ(web.handle(good).status, 200);

  // The UI itself must stay reachable, or there is no way to enter the token.
  EXPECT_EQ(web.handle({"GET", "/", "", ""}).status, 200);

  app.shutdown();
}

// Task 7: the auto-pair worker (and its background sweep thread) is wired into every StreamerApp,
// not just ones that open the provisioning window. This is a construction/shutdown smoke test — the
// window starts (and stays) closed here, so sweepOnce() is a no-op on every wake, no speaker is
// discovered or paired, and the thread must still start cleanly and join on shutdown with no crash
// and no hang. The worker's actual pairing behavior is covered by Task 6's AutoPairWorker tests.
TEST(StreamerApp, AutoPairWorkerStartsAndStopsCleanlyWithWindowClosed) {
  StreamerIdentityRig sid("autopair");
  SpeakerRig rig("autopair", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::send::MemoryPacketSink sink;
  nexus::web::StubWebTransport web;

  streamer::app::StreamerApp::Options opts;
  opts.enable_link_reporter = false;
  streamer::app::StreamerApp app(*sid.id, line, sink, web, opts);
  ASSERT_TRUE(app.startup().ok());

  // Provisioning window is closed by default — give the sweep thread a couple of its ~3 s wake
  // intervals to prove it stays quiet rather than crashing or registering anything.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(app.registry().size(), 0u) << "no speaker should be auto-paired with the window closed";

  // Shutdown must stop and join the sweep thread promptly, not hang waiting out its interval.
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(app.shutdown().ok());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(2))
      << "shutdown should join the auto-pair thread immediately via its condition variable";
}

TEST(StreamerApp, StartStopIsRepeatableAndLeaksNoThreads) {
  StreamerIdentityRig sid("cycle");
  SpeakerRig rig("cycle", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::send::MemoryPacketSink sink;
  nexus::web::StubWebTransport web;

  for (int i = 0; i < 20; ++i) {
    streamer::app::StreamerApp app(*sid.id, line, sink, web);
    ASSERT_TRUE(app.startup().ok()) << "cycle " << i;
    EXPECT_EQ(app.audio().state(), core::ServiceState::Running);
    ASSERT_TRUE(app.shutdown().ok()) << "cycle " << i;
    EXPECT_EQ(app.audio().state(), core::ServiceState::Stopped);
    EXPECT_TRUE(app.shutdown().ok());  // idempotent
  }
}

TEST(StreamerApp, PauseAndResumeAreDrivenFromTheApi) {
  using streamer::audio::AudioEngine;
  StreamerIdentityRig sid("pause");
  SpeakerRig rig("pause", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::send::MemoryPacketSink sink;
  nexus::web::StubWebTransport web;

  streamer::app::StreamerApp::Options opts;
  opts.enable_link_reporter = false;
  streamer::app::StreamerApp app(*sid.id, line, sink, web, opts);
  ASSERT_TRUE(app.startup().ok());
  app.registry().upsert({rig.id->deviceId(), "Kitchen", "127.0.0.1", 45455});

  // A live-ish source: long enough that it does not finish during the test.
  ASSERT_TRUE(app.playTo({{"127.0.0.1", nexus::audio::wire::kDefaultPort}},
                         std::make_unique<streamer::sources::MemoryAudioSource>(tone(480000), 2))
                  .ok());
  EXPECT_EQ(app.audio().transport().state, AudioEngine::State::Playing);

  auto paused = web.handle(post("/api/transport", {{"speaker", rig.id->deviceId()}, {"action", "pause"}}, app.authToken()));
  EXPECT_EQ(paused.status, 200);
  EXPECT_EQ(app.audio().transport().state, AudioEngine::State::Paused);

  // Nothing is emitted while paused.
  const auto at_pause = sink.count();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_EQ(sink.count(), at_pause);

  auto resumed = web.handle(post("/api/transport", {{"speaker", rig.id->deviceId()}, {"action", "resume"}}, app.authToken()));
  EXPECT_EQ(resumed.status, 200);
  EXPECT_EQ(app.audio().transport().state, AudioEngine::State::Playing);

  app.shutdown();
}

// command_id uniqueness (pulled forward from Phase 7b): the speaker checks its idempotency cache
// BEFORE verifying the signature and returns the cached result for a repeated id, so a colliding id
// silently reports a stale value. The old scheme used one-second granularity, which a dragged
// volume slider collides with several times per second.
TEST(CommandGateway, CommandIdsAreUniqueUnderRapidFire) {
  StreamerIdentityRig sid("ids");
  SpeakerRig rig("ids", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::app::CommandGateway gateway(line, sid.id->secretKeyBase64(), sid.id->streamerId());

  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(seen.insert(gateway.nextCommandId("SET_VOLUME")).second)
        << "duplicate command_id at iteration " << i;
  }
  EXPECT_EQ(seen.size(), 1000u);
}

// Two rapid volume sets must both reach the speaker rather than the second being swallowed by the
// idempotency cache and echoing the first one's value.
TEST(CommandGateway, RapidVolumeChangesEachTakeEffect) {
  StreamerIdentityRig sid("rapid");
  SpeakerRig rig("rapid", sid.id->publicKeyBase64());
  LoopbackLineTransport line(rig.transport);
  streamer::app::CommandGateway gateway(line, sid.id->secretKeyBase64(), sid.id->streamerId(),
                                        [&] { return rig.now; });

  const streamer::group::Speaker target{rig.id->deviceId(), "Den", "127.0.0.1", 45455};
  for (int v : {10, 20, 30, 40, 50}) {
    auto reply = gateway.send(target, "SET_VOLUME", {{"volume", v}});
    ASSERT_TRUE(reply.ok()) << reply.status().message();
    ASSERT_TRUE(reply.value().ok) << reply.value().message;
    EXPECT_EQ(reply.value().data.value("volume", -1), v);
  }
  EXPECT_EQ(rig.config->get().audio.volume, 50);  // the last one won, not the first
}
