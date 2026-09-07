#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "identity/Crypto.h"
#include "identity/KeyManager.h"
#include "pairing/PairingService.h"
#include "storage/SecureStorage.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_pair_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// Simulates the Streamer side: holds an Ed25519 keypair, can seal Wi-Fi creds to the speaker's
// X25519 public key and sign a pairing request.
struct FakeStreamer {
  std::vector<std::uint8_t> pk, sk;  // Ed25519
  // Must satisfy isWellFormedStreamerId ("STR-" + 8 lowercase hex) — PairingValidator now
  // enforces the shape, so the default fixture id has to be well-formed too.
  std::string id = "STR-1ab01234";

  FakeStreamer() {
    pk.resize(crypto_sign_PUBLICKEYBYTES);
    sk.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_keypair(pk.data(), sk.data());
  }
  std::string pkB64() const { return identity::crypto::toBase64(pk); }

  pairing::PairingRequest makeRequest(const std::string& speaker_box_pub_b64,
                                      const std::string& setup_code, const std::string& ssid,
                                      const std::string& psk) {
    pairing::PairingRequest req;
    req.streamer_id = id;
    req.streamer_public_key = pkB64();
    req.site_id = "site-1";
    req.initial_speaker_name = "Living Room";
    req.setup_code = setup_code;

    // Seal {ssid,psk} to the speaker's X25519 public key.
    nlohmann::json wj = {{"ssid", ssid}, {"psk", psk}};
    std::string ws = wj.dump();
    auto boxpub = identity::crypto::fromBase64(speaker_box_pub_b64);
    auto sealed = identity::crypto::sealTo(
        boxpub.value(), std::vector<std::uint8_t>(ws.begin(), ws.end()));
    req.sealed_wifi_b64 = identity::crypto::toBase64(sealed.value());

    // Sign the canonical bytes.
    std::string canonical = req.canonicalString();
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr,
                         reinterpret_cast<const unsigned char*>(canonical.data()),
                         canonical.size(), sk.data());
    req.signature_b64 = identity::crypto::toBase64(sig);
    return req;
  }
};

struct Rig {
  fs::path dir;
  core::EventBus bus;
  std::unique_ptr<storage::SecureStorage> secrets;
  std::unique_ptr<identity::KeyManager> keys;
  std::unique_ptr<config::ConfigManager> config;
  std::unique_ptr<pairing::PairingService> svc;

  explicit Rig(const std::string& name) : dir(sandbox(name)) {
    secrets = std::make_unique<storage::SecureStorage>((dir / "secure").string());
    keys = std::make_unique<identity::KeyManager>(secrets.get());
    keys->generateIfMissing();
    config = std::make_unique<config::ConfigManager>((dir / "config.json").string(), &bus);
    config->start();
    svc = std::make_unique<pairing::PairingService>(&bus, keys.get(), config.get(),
                                                    secrets.get());
    svc->start();
  }
};

}  // namespace

TEST(Pairing, SuccessfulPairingPersistsAndDecryptsWifi) {
  Rig rig("ok");
  FakeStreamer streamer;
  auto boxpub = rig.keys->boxPublicKeyBase64();
  ASSERT_TRUE(boxpub.ok());

  ASSERT_TRUE(rig.svc->beginSetupMode("123456", /*now*/ 1000, /*ttl*/ 300).ok());
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "wifi-secret-pw");

  int completed = 0;
  rig.bus.subscribe(core::EventType::PairingCompleted, [&](const core::Event&) { ++completed; });

  auto res = rig.svc->processRequest(req, /*now*/ 1100);
  rig.bus.drain();
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.value().streamer_id, "STR-1ab01234");
  EXPECT_EQ(res.value().wifi.ssid, "NexusLab");
  EXPECT_EQ(res.value().wifi.psk, "wifi-secret-pw");
  EXPECT_EQ(completed, 1);

  // Persisted: config has public pairing info + flag; secrets hold the PSK.
  EXPECT_TRUE(rig.config->get().pairing.paired);
  EXPECT_EQ(rig.config->get().pairing.streamer_id, "STR-1ab01234");
  EXPECT_EQ(rig.config->get().pairing.streamer_public_key, streamer.pkB64());
  EXPECT_TRUE(rig.secrets->has(pairing::PairingService::kWifiPskSecret));

  // The PSK must NOT appear in the config file on disk.
  std::ifstream in((rig.dir / "config.json"));
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents.find("wifi-secret-pw"), std::string::npos);
}

TEST(Pairing, RejectsWrongSetupCode) {
  Rig rig("badcode");
  FakeStreamer streamer;
  auto boxpub = rig.keys->boxPublicKeyBase64();
  rig.svc->beginSetupMode("111111", 1000, 300);
  auto req = streamer.makeRequest(boxpub.value(), "999999", "NexusLab", "pw");
  auto res = rig.svc->processRequest(req, 1100);
  EXPECT_FALSE(res.ok());
  EXPECT_FALSE(rig.config->get().pairing.paired);
}

TEST(Pairing, RejectsExpiredCode) {
  Rig rig("expired");
  FakeStreamer streamer;
  auto boxpub = rig.keys->boxPublicKeyBase64();
  rig.svc->beginSetupMode("123456", 1000, 60);  // expires at 1060
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "pw");
  auto res = rig.svc->processRequest(req, 5000);  // well past expiry
  EXPECT_FALSE(res.ok());
  EXPECT_FALSE(rig.config->get().pairing.paired);
}

TEST(Pairing, RejectsTamperedSignature) {
  Rig rig("tamper");
  FakeStreamer streamer;
  auto boxpub = rig.keys->boxPublicKeyBase64();
  rig.svc->beginSetupMode("123456", 1000, 300);
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "pw");
  // Tamper: change site_id after signing so the canonical bytes no longer match the signature.
  req.site_id = "attacker-site";
  auto res = rig.svc->processRequest(req, 1100);
  EXPECT_FALSE(res.ok());
  EXPECT_FALSE(rig.config->get().pairing.paired);
}

TEST(Pairing, RejectsRequestWhenNotInSetupMode) {
  Rig rig("nosetup");
  FakeStreamer streamer;
  auto boxpub = rig.keys->boxPublicKeyBase64();
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "pw");
  auto res = rig.svc->processRequest(req, 1100);  // never called beginSetupMode
  EXPECT_FALSE(res.ok());
}

TEST(Pairing, RejectsMalformedStreamerId) {
  Rig rig("malformedid");
  FakeStreamer streamer;
  streamer.id = "STR-BADID";  // wrong shape: not "STR-" + 8 lowercase hex
  auto boxpub = rig.keys->boxPublicKeyBase64();
  rig.svc->beginSetupMode("123456", 1000, 300);
  // Signed over the malformed id itself, so this isn't rejected on signature grounds.
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "pw");
  auto res = rig.svc->processRequest(req, 1100);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("streamer_id"), std::string::npos);
  EXPECT_FALSE(rig.config->get().pairing.paired);
}

TEST(Pairing, AcceptsWellFormedStreamerId) {
  Rig rig("wellformedid");
  FakeStreamer streamer;
  streamer.id = "STR-deadbeef";  // well-formed: "STR-" + 8 lowercase hex
  auto boxpub = rig.keys->boxPublicKeyBase64();
  rig.svc->beginSetupMode("123456", 1000, 300);
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "pw");
  auto res = rig.svc->processRequest(req, 1100);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.value().streamer_id, "STR-deadbeef");
}

TEST(Pairing, PairingIsOneTime) {
  Rig rig("onetime");
  FakeStreamer streamer;
  auto boxpub = rig.keys->boxPublicKeyBase64();
  rig.svc->beginSetupMode("123456", 1000, 300);
  auto req = streamer.makeRequest(boxpub.value(), "123456", "NexusLab", "pw");
  ASSERT_TRUE(rig.svc->processRequest(req, 1100).ok());
  EXPECT_FALSE(rig.svc->inSetupMode());
  // A second request must be rejected (no longer in setup mode).
  auto res2 = rig.svc->processRequest(req, 1150);
  EXPECT_FALSE(res2.ok());
}
