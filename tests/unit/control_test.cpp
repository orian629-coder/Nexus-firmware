#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "control/CommandServer.h"
#include "control/ICommandTransport.h"
#include "core/EventBus.h"
#include "identity/Crypto.h"
#include "identity/DeviceIdentity.h"
#include "storage/SecureStorage.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_ctl_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// Simulates the Streamer signing commands with its Ed25519 key.
struct FakeStreamer {
  std::vector<std::uint8_t> pk, sk;
  FakeStreamer() {
    pk.resize(crypto_sign_PUBLICKEYBYTES);
    sk.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_keypair(pk.data(), sk.data());
  }
  std::string pkB64() const { return identity::crypto::toBase64(pk); }

  std::string sign(control::Command& c) {
    std::string canon = c.canonicalString();
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(canon.data()),
                         canon.size(), sk.data());
    c.signature_b64 = identity::crypto::toBase64(sig);
    return c.signature_b64;
  }
};

// A test rig: identity + config (paired to the fake streamer) + a CommandServer with a stub
// transport so we can inject requests via deliver().
struct Rig {
  fs::path dir;
  core::EventBus bus;
  std::unique_ptr<storage::SecureStorage> secrets;
  std::unique_ptr<identity::DeviceIdentity> id;
  std::unique_ptr<config::ConfigManager> config;
  std::unique_ptr<control::CommandServer> server;
  control::StubCommandTransport* transport = nullptr;
  FakeStreamer streamer;
  std::int64_t now = 1000;

  explicit Rig(const std::string& name) : dir(sandbox(name)) {
    secrets = std::make_unique<storage::SecureStorage>((dir / "secure").string());
    id = std::make_unique<identity::DeviceIdentity>((dir / "identity/factory.json").string(),
                                                    secrets.get(), &bus);
    id->start();
    config = std::make_unique<config::ConfigManager>((dir / "config.json").string(), &bus);
    config->start();
    // Pair to the fake streamer.
    config->update([&](config::SpeakerConfig& c) {
      c.pairing.paired = true;
      c.pairing.streamer_id = "STR-1";
      c.pairing.streamer_public_key = streamer.pkB64();
    });
    auto t = std::make_unique<control::StubCommandTransport>();
    transport = t.get();
    server = std::make_unique<control::CommandServer>(&bus, id.get(), config.get(),
                                                      [this] { return now; }, std::move(t));
    server->start();
  }

  control::Command makeCommand(const std::string& name, nlohmann::json payload = {}) {
    control::Command c;
    c.command_id = "cmd-" + name;
    c.target_id = id->deviceId();
    c.command = name;
    if (!payload.is_null()) c.payload = payload;
    c.timestamp = now;
    c.expires_at = now + 30;
    streamer.sign(c);
    return c;
  }

  nlohmann::json send(const control::Command& c) {
    std::string resp = transport->deliver(c.toJson().dump());
    return nlohmann::json::parse(resp);
  }
};

}  // namespace

TEST(Control, SignedSetVolumeSucceedsAndPersists) {
  Rig rig("volume");
  auto c = rig.makeCommand("SET_VOLUME", {{"volume", 65}});
  auto resp = rig.send(c);
  EXPECT_TRUE(resp["ok"].get<bool>());
  EXPECT_EQ(rig.config->get().audio.volume, 65);
}

TEST(Control, UnsignedCommandRejected) {
  Rig rig("unsigned");
  auto c = rig.makeCommand("SET_VOLUME", {{"volume", 50}});
  c.signature_b64 = "";  // strip the signature
  auto resp = rig.send(c);
  EXPECT_FALSE(resp["ok"].get<bool>());
  EXPECT_NE(rig.config->get().audio.volume, 50);
}

TEST(Control, TamperedCommandRejected) {
  Rig rig("tamper");
  auto c = rig.makeCommand("SET_VOLUME", {{"volume", 10}});
  c.payload["volume"] = 99;  // change after signing
  auto resp = rig.send(c);
  EXPECT_FALSE(resp["ok"].get<bool>());
}

TEST(Control, ExpiredCommandRejected) {
  Rig rig("expired");
  auto c = rig.makeCommand("SET_MUTE", {{"muted", true}});
  rig.now = 100000;  // far past expires_at
  auto resp = rig.send(c);
  EXPECT_FALSE(resp["ok"].get<bool>());
}

TEST(Control, WrongTargetRejected) {
  Rig rig("target");
  auto c = rig.makeCommand("SET_MUTE", {{"muted", true}});
  c.target_id = "SPK-OTHER";
  rig.streamer.sign(c);  // re-sign so signature matches the tampered target
  auto resp = rig.send(c);
  EXPECT_FALSE(resp["ok"].get<bool>());
}

TEST(Control, DuplicateCommandIdIsIdempotent) {
  Rig rig("dup");
  auto c = rig.makeCommand("SET_VOLUME", {{"volume", 40}});
  auto r1 = rig.send(c);
  EXPECT_TRUE(r1["ok"].get<bool>());

  // Change the config underneath, then replay the same command_id: it must return the cached
  // result WITHOUT re-executing (volume stays where the replay-free path left it).
  rig.config->update([](config::SpeakerConfig& cfg) { cfg.audio.volume = 5; });
  auto r2 = rig.send(c);
  EXPECT_TRUE(r2["ok"].get<bool>());
  EXPECT_EQ(rig.config->get().audio.volume, 5);  // not overwritten back to 40 by a re-exec
}

// REPORT_LINK is telemetry the streamer pushes every ~2 s. It must NOT consume idempotency slots:
// the history is a bounded LRU (200), so caching it would evict every real command's record within
// minutes and silently break the retry-safety that the cache exists to provide.
TEST(Control, ReportLinkDoesNotEvictRealCommandsFromIdempotencyCache) {
  Rig rig("linkflood");

  // A real command whose result must stay cached.
  auto vol = rig.makeCommand("SET_VOLUME", {{"volume", 40}});
  ASSERT_TRUE(rig.send(vol)["ok"].get<bool>());

  // More telemetry than the entire history capacity.
  for (int i = 0; i < 250; ++i) {
    control::Command link;
    link.command_id = "link-" + std::to_string(i);
    link.target_id = rig.id->deviceId();
    link.command = "REPORT_LINK";
    link.payload = {{"wifi_signal_dbm", -50}, {"streamer_id", "STR-1"}};
    link.timestamp = rig.now;
    link.expires_at = rig.now + 30;
    rig.streamer.sign(link);
    ASSERT_TRUE(rig.send(link)["ok"].get<bool>()) << "telemetry rejected at i=" << i;
  }

  // Replaying the original command_id must still hit the cache rather than re-executing.
  rig.config->update([](config::SpeakerConfig& cfg) { cfg.audio.volume = 5; });
  auto replay = rig.send(vol);
  EXPECT_TRUE(replay["ok"].get<bool>());
  EXPECT_EQ(rig.config->get().audio.volume, 5)
      << "telemetry flushed the idempotency cache; the replay re-executed";
}

TEST(Control, GetStatusReturnsState) {
  Rig rig("status");
  rig.config->update([](config::SpeakerConfig& cfg) { cfg.audio.volume = 33; });
  auto c = rig.makeCommand("GET_STATUS", nlohmann::json::object());
  auto resp = rig.send(c);
  ASSERT_TRUE(resp["ok"].get<bool>());
  EXPECT_EQ(resp["data"]["volume"].get<int>(), 33);
}

TEST(Control, UnknownCommandRejected) {
  Rig rig("unknown");
  auto c = rig.makeCommand("DANCE", nlohmann::json::object());
  auto resp = rig.send(c);
  EXPECT_FALSE(resp["ok"].get<bool>());
}

TEST(Control, ReportLinkRecordsStreamerSignal) {
  Rig rig("reportlink");
  auto c = rig.makeCommand("REPORT_LINK", {{"wifi_signal_dbm", -57}, {"streamer_id", "STR-1"}});
  auto resp = rig.send(c);
  ASSERT_TRUE(resp["ok"].get<bool>());
  EXPECT_EQ(resp["data"]["wifi_signal_dbm"].get<int>(), -57);
  // The reported signal lands in LinkState (what the status API reads for the kiosk meter), stamped
  // with the injected clock so freshness is computable.
  auto snap = rig.server->linkState()->get();
  EXPECT_TRUE(snap.valid);
  EXPECT_EQ(snap.wifi_signal_dbm, -57);
  EXPECT_EQ(snap.streamer_id, "STR-1");
  EXPECT_EQ(snap.reported_at, rig.now);
}

TEST(Control, ReportLinkWithoutSignalIsAckedButNotRecorded) {
  Rig rig("reportlink-empty");
  // A wired streamer has no RSSI to report; the command is still accepted (so it isn't retried) but
  // nothing is recorded, leaving the meter to fall back to the speaker's own RTT measurement.
  auto c = rig.makeCommand("REPORT_LINK", {{"streamer_id", "STR-1"}});
  auto resp = rig.send(c);
  ASSERT_TRUE(resp["ok"].get<bool>());
  EXPECT_FALSE(resp["data"]["recorded"].get<bool>());
  EXPECT_FALSE(rig.server->linkState()->get().valid);
}

TEST(Control, MalformedJsonRejected) {
  Rig rig("malformed");
  std::string resp = rig.transport->deliver("{ not valid json");
  auto j = nlohmann::json::parse(resp);
  EXPECT_FALSE(j["ok"].get<bool>());
}

TEST(Control, PairingRequestRoutedToHandler) {
  Rig rig("pairing");
  int called = 0;
  rig.server->setPairingHandler([&](const std::string& raw) -> std::string {
    ++called;
    auto j = nlohmann::json::parse(raw);
    EXPECT_EQ(j.value("type", ""), "pairing");
    return R"({"type":"pairing_result","ok":true})";
  });
  // A type=="pairing" message bypasses the signed-command pipeline and reaches the handler.
  std::string resp = rig.transport->deliver(R"({"type":"pairing","streamer_id":"STR-9"})");
  auto j = nlohmann::json::parse(resp);
  EXPECT_EQ(called, 1);
  EXPECT_TRUE(j["ok"].get<bool>());
  EXPECT_EQ(j["type"], "pairing_result");
}

TEST(Control, NonPairingStillGoesThroughCommandPipeline) {
  Rig rig("nonpairing");
  rig.server->setPairingHandler([&](const std::string&) -> std::string {
    ADD_FAILURE() << "pairing handler must not see normal commands";
    return "{}";
  });
  auto c = rig.makeCommand("SET_VOLUME", {{"volume", 33}});
  auto resp = rig.send(c);
  EXPECT_TRUE(resp["ok"].get<bool>());
}
