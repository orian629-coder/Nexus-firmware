#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>
#include <memory>

#include "config/ConfigManager.h"
#include "control/CommandClient.h"
#include "control/CommandServer.h"
#include "control/ICommandTransport.h"
#include "control/ILineTransport.h"
#include "core/EventBus.h"
#include "identity/Crypto.h"
#include "identity/DeviceIdentity.h"
#include "storage/SecureStorage.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_ctl_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// Fixed streamer keypair (base64), so the same key signs commands (streamer side) and is stored as
// the speaker's paired streamer_public_key (speaker side).
struct StreamerKey {
  std::string sk_b64, pk_b64;
  StreamerKey() {
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    sk_b64 = identity::crypto::toBase64(std::vector<std::uint8_t>(sk, sk + sizeof(sk)));
    pk_b64 = identity::crypto::toBase64(std::vector<std::uint8_t>(pk, pk + sizeof(pk)));
  }
};

// An ILineTransport that forwards the request straight into the speaker's real CommandServer via the
// StubCommandTransport's deliver() hook — so the streamer's CommandClient exercises the REAL server
// pipeline (signature verification, routing, config persistence) with no sockets. Host/port are
// ignored (single in-process speaker).
class LoopbackLineTransport : public streamer::control::ILineTransport {
 public:
  explicit LoopbackLineTransport(control::StubCommandTransport* t) : transport_(t) {}
  core::Result<std::string> request(const std::string&, int, const std::string& req) override {
    return transport_->deliver(req);
  }

 private:
  control::StubCommandTransport* transport_;
};

// Brings up a real, paired speaker CommandServer in-process.
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

// The Phase-2 proof: the streamer's real CommandClient (signing + client logic) drives the real
// speaker CommandServer and gets an ok result, and the effect is persisted in the speaker's config.
TEST(CommandClient, SetVolumeAcceptedByRealSpeakerServer) {
  StreamerKey key;
  SpeakerRig rig("setvol", key.pk_b64);
  LoopbackLineTransport transport(rig.transport);
  streamer::control::CommandSigner signer(key.sk_b64);
  streamer::control::CommandClient client(transport, signer, "ignored-host", 45455);

  const std::string target = rig.id->deviceId();
  auto reply = client.send("cmd-1", target, "SET_VOLUME", {{"volume", 55}}, rig.now + 30, rig.now);
  ASSERT_TRUE(reply.ok()) << reply.status().message();
  EXPECT_TRUE(reply.value().ok) << reply.value().message;
  EXPECT_EQ(reply.value().command_id, "cmd-1");
  EXPECT_EQ(rig.config->get().audio.volume, 55);  // persisted on the speaker
}

TEST(CommandClient, GetStatusReturnsData) {
  StreamerKey key;
  SpeakerRig rig("status", key.pk_b64);
  LoopbackLineTransport transport(rig.transport);
  streamer::control::CommandClient client(transport, streamer::control::CommandSigner(key.sk_b64),
                                          "h", 45455);

  auto reply = client.send("cmd-s", rig.id->deviceId(), "GET_STATUS", {}, rig.now + 30, rig.now);
  ASSERT_TRUE(reply.ok());
  EXPECT_TRUE(reply.value().ok) << reply.value().message;
  EXPECT_TRUE(reply.value().data.contains("volume"));
}

// Wrong key → the real server rejects the signature; the client surfaces ok=false, not a crash.
TEST(CommandClient, WrongKeyRejectedByServer) {
  StreamerKey paired, imposter;
  SpeakerRig rig("badkey", paired.pk_b64);  // speaker paired to `paired`
  LoopbackLineTransport transport(rig.transport);
  streamer::control::CommandClient client(transport,
                                          streamer::control::CommandSigner(imposter.sk_b64),  // wrong
                                          "h", 45455);

  auto reply = client.send("cmd-x", rig.id->deviceId(), "SET_MUTE", {{"muted", true}}, rig.now + 30,
                           rig.now);
  ASSERT_TRUE(reply.ok());          // transport succeeded
  EXPECT_FALSE(reply.value().ok);   // but the command was rejected
}
