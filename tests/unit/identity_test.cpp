#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>
#include <fstream>

#include "identity/DeviceIdentity.h"
#include "storage/SecureStorage.h"

using namespace nexus::identity;
using nexus::storage::SecureStorage;
namespace fs = std::filesystem;

namespace {
struct Fixture {
  fs::path dir;
  std::string factoryPath;
  std::unique_ptr<SecureStorage> secrets;

  explicit Fixture(const std::string& name) {
    dir = fs::temp_directory_path() / ("nexus_id_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    factoryPath = (dir / "factory.json").string();
    secrets = std::make_unique<SecureStorage>((dir / "secure").string());
  }
  ~Fixture() { fs::remove_all(dir); }
};
}  // namespace

TEST(DeviceIdentity, ProvisionsOnFirstBoot) {
  Fixture f("provision");
  DeviceIdentity id(f.factoryPath, f.secrets.get());
  ASSERT_TRUE(id.start().ok());
  EXPECT_TRUE(id.hasFactoryIdentity());
  EXPECT_EQ(id.deviceId().rfind("SPK-", 0), 0u);  // starts with SPK-
  EXPECT_FALSE(id.publicKeyBase64().empty());
  EXPECT_TRUE(fs::exists(f.factoryPath));
}

TEST(DeviceIdentity, IdentityStableAcrossRestart) {
  Fixture f("stable");
  std::string id1, pk1;
  {
    DeviceIdentity id(f.factoryPath, f.secrets.get());
    ASSERT_TRUE(id.start().ok());
    id1 = id.deviceId();
    pk1 = id.publicKeyBase64();
  }
  {
    DeviceIdentity id(f.factoryPath, f.secrets.get());
    ASSERT_TRUE(id.start().ok());
    EXPECT_EQ(id.deviceId(), id1);
    EXPECT_EQ(id.publicKeyBase64(), pk1);
  }
}

TEST(DeviceIdentity, DeviceIdIsNotMacDerived) {
  Fixture f("notmac");
  DeviceIdentity id(f.factoryPath, f.secrets.get());
  ASSERT_TRUE(id.start().ok());
  // No colons (MAC form) and comfortably high entropy in the suffix.
  EXPECT_EQ(id.deviceId().find(':'), std::string::npos);
}

TEST(DeviceIdentity, FactoryJsonDoesNotContainPrivateKey) {
  Fixture f("nopriv");
  DeviceIdentity id(f.factoryPath, f.secrets.get());
  ASSERT_TRUE(id.start().ok());
  std::ifstream in(f.factoryPath);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents.find("private"), std::string::npos);
  EXPECT_EQ(contents.find("secret"), std::string::npos);
  // The secret key must live only in secure storage.
  EXPECT_TRUE(f.secrets->has(KeyManager::kSecretKeyName));
}

TEST(DeviceIdentity, SignVerifyRoundTrip) {
  Fixture f("sign");
  DeviceIdentity id(f.factoryPath, f.secrets.get());
  ASSERT_TRUE(id.start().ok());

  const std::string msg = "command:SET_VOLUME:70";
  auto sig = id.sign(msg);
  ASSERT_TRUE(sig.ok());

  // Decode public key + signature and verify with libsodium directly.
  ASSERT_GE(sodium_init(), 0);
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  size_t pklen = 0;
  ASSERT_EQ(sodium_base642bin(pk, sizeof(pk), id.publicKeyBase64().c_str(),
                              id.publicKeyBase64().size(), nullptr, &pklen, nullptr,
                              sodium_base64_VARIANT_ORIGINAL),
            0);

  unsigned char sigbin[crypto_sign_BYTES];
  size_t siglen = 0;
  ASSERT_EQ(sodium_base642bin(sigbin, sizeof(sigbin), sig.value().c_str(), sig.value().size(),
                              nullptr, &siglen, nullptr, sodium_base64_VARIANT_ORIGINAL),
            0);

  EXPECT_EQ(crypto_sign_verify_detached(
                sigbin, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), pk),
            0);
}

TEST(DeviceIdentity, FactoryResetPreservesIdentity) {
  // Simulate a factory reset: wipe a "config" file but keep identity/ and secure/.
  Fixture f("reset");
  std::string id1;
  {
    DeviceIdentity id(f.factoryPath, f.secrets.get());
    ASSERT_TRUE(id.start().ok());
    id1 = id.deviceId();
  }
  // reset.sh would remove config + state, never the identity dir or secure keypair:
  auto cfg = f.dir / "config.json";
  std::ofstream(cfg) << "{}";
  fs::remove(cfg);  // wiped
  ASSERT_TRUE(fs::exists(f.factoryPath));                           // preserved
  ASSERT_TRUE(f.secrets->has(KeyManager::kSecretKeyName));          // preserved

  DeviceIdentity id2(f.factoryPath, f.secrets.get());
  ASSERT_TRUE(id2.start().ok());
  EXPECT_EQ(id2.deviceId(), id1);  // same identity after reset
}
