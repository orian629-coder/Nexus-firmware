#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>
#include <memory>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "control/ILineTransport.h"
#include "core/EventBus.h"
#include "identity/Crypto.h"
#include "identity/KeyManager.h"
#include "pairing/PairingClient.h"
#include "pairing/PairingService.h"
#include "pairing/PairingTypes.h"
#include "storage/SecureStorage.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_pair_" + name);
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

// A real speaker pairing stack (KeyManager + PairingService) plus a transport that reproduces the
// speaker's raw-JSON pairing handler from Application.cpp: it maps the wire keys (sealed_wifi /
// signature) onto a PairingRequest and calls the REAL processRequest, returning the real
// pairing_result. This proves the streamer's PairingClient end to end (seal + sign + wire keys).
struct SpeakerPairingRig : public streamer::control::ILineTransport {
  fs::path dir;
  core::EventBus bus;
  std::unique_ptr<storage::SecureStorage> secrets;
  std::unique_ptr<identity::KeyManager> keys;
  std::unique_ptr<config::ConfigManager> config;
  std::unique_ptr<pairing::PairingService> svc;
  std::int64_t now = 1100;

  explicit SpeakerPairingRig(const std::string& name) : dir(sandbox(name)) {
    secrets = std::make_unique<storage::SecureStorage>((dir / "secure").string());
    keys = std::make_unique<identity::KeyManager>(secrets.get());
    keys->generateIfMissing();
    config = std::make_unique<config::ConfigManager>((dir / "config.json").string(), &bus);
    config->start();
    svc = std::make_unique<pairing::PairingService>(&bus, keys.get(), config.get(), secrets.get());
    svc->start();
  }

  std::string boxPub() { return keys->boxPublicKeyBase64().value(); }

  core::Result<std::string> request(const std::string&, int, const std::string& raw) override {
    pairing::PairingRequest req;
    try {
      auto j = nlohmann::json::parse(raw);
      req.streamer_id = j.value("streamer_id", "");
      req.streamer_public_key = j.value("streamer_public_key", "");
      req.site_id = j.value("site_id", "");
      req.initial_speaker_name = j.value("initial_speaker_name", "");
      req.setup_code = j.value("setup_code", "");
      req.sealed_wifi_b64 = j.value("sealed_wifi", "");
      req.signature_b64 = j.value("signature", "");
    } catch (const std::exception& e) {
      return nlohmann::json{{"type", "pairing_result"}, {"ok", false},
                            {"message", std::string("bad pairing json: ") + e.what()}}
          .dump();
    }
    auto res = svc->processRequest(req, now);
    if (!res.ok()) {
      return nlohmann::json{{"type", "pairing_result"}, {"ok", false},
                            {"message", res.status().message()}}
          .dump();
    }
    return nlohmann::json{{"type", "pairing_result"}, {"ok", true}, {"device_id", "SPK-TEST"}}
        .dump();
  }
};

streamer::pairing::PairingParams makeParams(SpeakerPairingRig& rig, const StreamerKey& key,
                                            const std::string& code) {
  streamer::pairing::PairingParams p;
  p.streamer_id = "STR-LAB01";
  p.streamer_public_key = key.pk_b64;
  p.streamer_secret_key = key.sk_b64;
  p.site_id = "site-1";
  p.initial_speaker_name = "Living Room";
  p.setup_code = code;
  p.wifi_ssid = "NexusLab";
  p.wifi_psk = "wifi-secret-pw";
  p.speaker_box_public_key = rig.boxPub();
  return p;
}

}  // namespace

// The Phase-2 pairing proof: the streamer's PairingClient seals + signs a request the real
// PairingService accepts, and the speaker persists the streamer identity.
TEST(PairingClient, SuccessfulPairingAcceptedByRealService) {
  StreamerKey key;
  SpeakerPairingRig rig("ok");
  ASSERT_TRUE(rig.svc->beginSetupMode("123456", /*now*/ 1000, /*ttl*/ 300).ok());

  streamer::pairing::PairingClient client(rig, "h", 45455);
  auto reply = client.pair(makeParams(rig, key, "123456"));
  ASSERT_TRUE(reply.ok()) << reply.status().message();
  EXPECT_TRUE(reply.value().ok) << reply.value().message;
  EXPECT_EQ(reply.value().device_id, "SPK-TEST");

  // Speaker persisted the streamer's public key.
  EXPECT_TRUE(rig.config->get().pairing.paired);
  EXPECT_EQ(rig.config->get().pairing.streamer_public_key, key.pk_b64);
}

TEST(PairingClient, WrongSetupCodeRejected) {
  StreamerKey key;
  SpeakerPairingRig rig("badcode");
  rig.svc->beginSetupMode("123456", 1000, 300);

  streamer::pairing::PairingClient client(rig, "h", 45455);
  auto reply = client.pair(makeParams(rig, key, "999999"));  // wrong code
  ASSERT_TRUE(reply.ok());
  EXPECT_FALSE(reply.value().ok);
  EXPECT_FALSE(rig.config->get().pairing.paired);
}
