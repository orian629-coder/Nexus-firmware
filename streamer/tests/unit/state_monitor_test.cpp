#include <gtest/gtest.h>

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/CommandGateway.h"
#include "config/ConfigManager.h"
#include "control/CommandServer.h"
#include "control/ICommandTransport.h"
#include "control/ILineTransport.h"
#include "core/EventBus.h"
#include "core/StreamerEvents.h"
#include "identity/Crypto.h"
#include "identity/DeviceIdentity.h"
#include "state/MonitorService.h"
#include "state/SpeakerStateStore.h"
#include "storage/SecureStorage.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_state_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

struct StreamerKey {
  std::string sk_b64, pk_b64;
  StreamerKey() {
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    sk_b64 = identity::crypto::toBase64(std::vector<std::uint8_t>(sk, sk + sizeof(sk)));
    pk_b64 = identity::crypto::toBase64(std::vector<std::uint8_t>(pk, pk + sizeof(pk)));
  }
};

// Drives the streamer's client code against the speaker's REAL CommandServer in-process. `reachable`
// simulates the speaker losing power: the transport fails rather than the speaker refusing.
class LoopbackLineTransport : public streamer::control::ILineTransport {
 public:
  explicit LoopbackLineTransport(control::StubCommandTransport* t) : transport_(t) {}

  core::Result<std::string> request(const std::string&, int, const std::string& req) override {
    if (!reachable.load()) {
      return core::Status::error(core::ErrorCode::IoError, "connection refused");
    }
    return transport_->deliver(req);
  }

  std::atomic<bool> reachable{true};

 private:
  control::StubCommandTransport* transport_;
};

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

}  // namespace

// ── THE source-of-truth test ─────────────────────────────────────────────────────────────────────
//
// The speaker's volume is changed ON THE SPEAKER (as the kiosk or its own web UI would), with the
// streamer sending no set at all. The streamer must still converge on the speaker's value: that is
// what "the speaker owns its state and the streamer mirrors it" means in practice.
TEST(Monitor, StreamerConvergesOnStateChangedOnTheSpeaker) {
  StreamerKey key;
  SpeakerRig rig("sot", key.pk_b64);
  LoopbackLineTransport line(rig.transport);
  core::EventBus bus;
  streamer::state::SpeakerStateStore store(&bus);
  streamer::group::SpeakerRegistry registry;
  streamer::app::CommandGateway gateway(line, key.sk_b64, "STR-1", [&] { return rig.now; });
  gateway.setReplyObserver([&](const std::string& id, const core::Status& st,
                               const streamer::control::CommandReply& reply) {
    if (!st.ok()) store.applyPollFailure(id, st.message());
    else if (reply.ok) store.applyConfirmedStatus(id, reply.data);
    else store.noteRejected(id, reply.message);
  });
  registry.upsert({rig.id->deviceId(), "Salon", "127.0.0.1", 45455});
  streamer::state::MonitorService monitor(gateway, registry, store);

  monitor.pollOnce();
  ASSERT_TRUE(store.get(rig.id->deviceId()).has_value());
  const int initial = store.get(rig.id->deviceId())->confirmed.volume;

  // Someone turns the speaker up locally — the streamer is not involved.
  rig.config->update([](config::SpeakerConfig& c) { c.audio.volume = 77; });
  EXPECT_NE(initial, 77);

  monitor.pollOnce();
  auto st = store.get(rig.id->deviceId());
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(st->confirmed.volume, 77) << "the streamer did not pick up the speaker's own change";
  EXPECT_TRUE(st->online);
}

// ── THE anti-optimism test ───────────────────────────────────────────────────────────────────────
//
// A command signed with the WRONG key is rejected by the speaker. The store must not move: the user
// asked for 90, the speaker never applied it, so 90 must appear nowhere.
TEST(Monitor, RejectedCommandNeverBecomesConfirmedState) {
  StreamerKey paired, attacker;
  SpeakerRig rig("reject", paired.pk_b64);
  LoopbackLineTransport line(rig.transport);
  core::EventBus bus;
  streamer::state::SpeakerStateStore store(&bus);
  streamer::group::SpeakerRegistry registry;

  // Establish a real baseline first, using the correctly-signed key.
  streamer::app::CommandGateway good(line, paired.sk_b64, "STR-1", [&] { return rig.now; });
  good.setReplyObserver([&](const std::string& id, const core::Status& st,
                            const streamer::control::CommandReply& reply) {
    if (st.ok() && reply.ok) store.applyConfirmedStatus(id, reply.data);
  });
  const streamer::group::Speaker target{rig.id->deviceId(), "Salon", "127.0.0.1", 45455};
  registry.upsert(target);
  ASSERT_TRUE(good.send(target, "SET_VOLUME", {{"volume", 30}}).ok());
  ASSERT_EQ(store.get(target.device_id)->confirmed.volume, 30);

  // Now a command the speaker will refuse (signed by a key it is not paired to).
  streamer::app::CommandGateway bad(line, attacker.sk_b64, "STR-1", [&] { return rig.now; });
  bad.setReplyObserver([&](const std::string& id, const core::Status& st,
                           const streamer::control::CommandReply& reply) {
    if (!st.ok()) store.applyPollFailure(id, st.message());
    else if (reply.ok) store.applyConfirmedStatus(id, reply.data);
    else store.noteRejected(id, reply.message);
  });
  auto reply = bad.send(target, "SET_VOLUME", {{"volume", 90}});
  ASSERT_TRUE(reply.ok());              // the exchange completed...
  EXPECT_FALSE(reply.value().ok);       // ...but the speaker refused it

  EXPECT_EQ(store.get(target.device_id)->confirmed.volume, 30)
      << "a rejected command leaked into confirmed state";
  EXPECT_EQ(rig.config->get().audio.volume, 30);  // and the speaker really is still at 30
  EXPECT_TRUE(store.get(target.device_id)->online);  // it answered, so it is alive
}

// A speaker that dies silently must be detected by polling alone — no user action involved. This is
// what the old registry `online` flag could not do.
TEST(Monitor, SilentlyDeadSpeakerGoesOfflineAndRecovers) {
  StreamerKey key;
  SpeakerRig rig("offline", key.pk_b64);
  LoopbackLineTransport line(rig.transport);
  core::EventBus bus;
  streamer::state::SpeakerStateStore store(&bus);
  streamer::group::SpeakerRegistry registry;
  streamer::app::CommandGateway gateway(line, key.sk_b64, "STR-1", [&] { return rig.now; });
  gateway.setReplyObserver([&](const std::string& id, const core::Status& st,
                               const streamer::control::CommandReply& reply) {
    if (!st.ok()) store.applyPollFailure(id, st.message(), 3);
    else if (reply.ok) store.applyConfirmedStatus(id, reply.data);
    else store.noteRejected(id, reply.message);
  });
  const std::string dev = rig.id->deviceId();
  registry.upsert({dev, "Salon", "127.0.0.1", 45455});
  streamer::state::MonitorService monitor(gateway, registry, store);

  monitor.pollOnce();
  ASSERT_TRUE(store.get(dev)->online);

  line.reachable = false;  // power cut — nothing announces it

  monitor.pollOnce();
  EXPECT_TRUE(store.get(dev)->online) << "one missed poll must not flap the speaker offline";
  monitor.pollOnce();
  EXPECT_TRUE(store.get(dev)->online);
  monitor.pollOnce();
  EXPECT_FALSE(store.get(dev)->online) << "three consecutive failures should mark it offline";
  EXPECT_FALSE(store.get(dev)->last_error.empty());

  // Confirmed values survive going offline — they are the last known truth, just stale.
  EXPECT_TRUE(store.get(dev)->confirmed.everConfirmed());

  line.reachable = true;
  monitor.pollOnce();
  EXPECT_TRUE(store.get(dev)->online) << "it should come back on the first successful poll";
  EXPECT_EQ(store.get(dev)->consecutive_failures, 0);
}

// Telemetry must not decide reachability. REPORT_LINK fires every ~2 s against a 5 s poll and a
// 3-failure threshold; if its failures reached the store, an unreachable speaker would be declared
// offline on the telemetry clock — roughly 2.5x sooner than configured — and a brief blip would
// look like a dead speaker.
TEST(Monitor, LinkTelemetryFailuresDoNotDriveOfflineDetection) {
  StreamerKey key;
  SpeakerRig rig("telemetry", key.pk_b64);
  LoopbackLineTransport line(rig.transport);
  core::EventBus bus;
  streamer::state::SpeakerStateStore store(&bus);
  streamer::app::CommandGateway gateway(line, key.sk_b64, "STR-1", [&] { return rig.now; });
  gateway.setReplyObserver([&](const std::string& id, const core::Status& st,
                               const streamer::control::CommandReply& reply) {
    if (!st.ok()) store.applyPollFailure(id, st.message(), 3);
    else if (reply.ok) store.applyConfirmedStatus(id, reply.data);
    else store.noteRejected(id, reply.message);
  });
  const streamer::group::Speaker target{rig.id->deviceId(), "Salon", "127.0.0.1", 45455};
  ASSERT_TRUE(gateway.send(target, "GET_STATUS", {}).ok());
  ASSERT_TRUE(store.get(target.device_id)->online);

  line.reachable = false;

  // Far more failed telemetry pushes than the offline threshold.
  for (int i = 0; i < 10; ++i) {
    gateway.sendUnobserved(target, "REPORT_LINK", {{"wifi_signal_dbm", -55}});
  }
  EXPECT_TRUE(store.get(target.device_id)->online)
      << "telemetry failures leaked into reachability";
  EXPECT_EQ(store.get(target.device_id)->consecutive_failures, 0);

  // The status poll is what actually decides.
  for (int i = 0; i < 3; ++i) gateway.send(target, "GET_STATUS", {});
  EXPECT_FALSE(store.get(target.device_id)->online);
}

// The nine commands the speaker acks with ok:true but does not act on must not confirm anything.
TEST(StateStore, DeferredAckDoesNotConfirmState) {
  streamer::state::SpeakerStateStore store;
  store.applyConfirmedStatus("SPK-1", {{"volume", 42}, {"muted", false}});
  ASSERT_EQ(store.get("SPK-1")->confirmed.volume, 42);

  // START_AUDIO's real reply shape.
  store.applyConfirmedStatus("SPK-1", {{"accepted", true}, {"deferred", true}});
  EXPECT_EQ(store.get("SPK-1")->confirmed.volume, 42) << "a deferred ack changed confirmed state";
}

// A partial reply (e.g. SET_VOLUME's echo) must update only the key it carries.
TEST(StateStore, PartialReplyUpdatesOnlyWhatItCarries) {
  streamer::state::SpeakerStateStore store;
  store.applyConfirmedStatus(
      "SPK-1", {{"volume", 20}, {"muted", true}, {"delay_ms", 5}, {"eq_profile", "flat"}});
  store.applyConfirmedStatus("SPK-1", {{"volume", 65}});

  auto st = store.get("SPK-1");
  EXPECT_EQ(st->confirmed.volume, 65);
  EXPECT_TRUE(st->confirmed.muted);              // untouched
  EXPECT_EQ(st->confirmed.delay_ms, 5);          // untouched
  EXPECT_EQ(st->confirmed.eq_profile, "flat");   // untouched
}

TEST(StateStore, PublishesOnlineAndOfflineTransitions) {
  core::EventBus bus;
  std::atomic<int> online{0}, offline{0};
  bus.subscribeAll([&](const core::Event& ev) {
    if (streamer::events::is(ev, streamer::events::kSpeakerOnline)) online.fetch_add(1);
    if (streamer::events::is(ev, streamer::events::kSpeakerOffline)) offline.fetch_add(1);
  });
  streamer::state::SpeakerStateStore store(&bus);

  store.applyConfirmedStatus("SPK-1", {{"volume", 10}});
  store.applyConfirmedStatus("SPK-1", {{"volume", 11}});  // still online: no second event
  for (int i = 0; i < 3; ++i) store.applyPollFailure("SPK-1", "timeout", 3);

  // The bus dispatches on its own worker thread.
  for (int i = 0; i < 100 && (online.load() < 1 || offline.load() < 1); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(online.load(), 1) << "online should fire once, on the transition";
  EXPECT_EQ(offline.load(), 1);
}
