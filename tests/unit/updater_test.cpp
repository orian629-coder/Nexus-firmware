#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "core/EventBus.h"
#include "identity/Crypto.h"
#include "updater/UpdateManager.h"

using namespace nexus;
using namespace nexus::updater;
namespace fs = std::filesystem;

namespace {

// A fake vendor that signs packages with its Ed25519 key.
struct Vendor {
  std::vector<std::uint8_t> pk, sk;
  Vendor() {
    pk.resize(crypto_sign_PUBLICKEYBYTES);
    sk.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_keypair(pk.data(), sk.data());
  }
  std::string pkB64() const { return identity::crypto::toBase64(pk); }

  UpdatePackage makePackage(const std::string& version, const std::vector<std::uint8_t>& payload,
                            const std::string& min_hw = "1.0") {
    UpdatePackage pkg;
    pkg.payload = payload;
    pkg.manifest.version = version;
    pkg.manifest.min_hardware_version = min_hw;
    pkg.manifest.payload_sha256 = identity::crypto::sha256Hex(payload);
    pkg.manifest.size_bytes = static_cast<std::int64_t>(payload.size());
    std::string canon = pkg.manifest.canonicalString();
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(canon.data()),
                         canon.size(), sk.data());
    pkg.signature_b64 = identity::crypto::toBase64(sig);
    return pkg;
  }
};

std::string writeCurrentBinary(const fs::path& dir) {
  auto p = dir / "nexus-speaker";
  std::ofstream(p) << "OLD_BINARY_V1";
  return p.string();
}

std::vector<std::uint8_t> bytesOf(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

TEST(UpdateManager, SuccessfulSignedUpdateInstallsAndCompletes) {
  ASSERT_GE(sodium_init(), 0);
  auto dir = fs::temp_directory_path() / "nexus_upd_ok";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto target = writeCurrentBinary(dir);

  Vendor vendor;
  auto src = std::make_unique<StubUpdateSource>();
  auto* src_ptr = src.get();
  auto newPayload = bytesOf("NEW_BINARY_V2_CONTENTS");
  src_ptr->package = vendor.makePackage("2.0.0", newPayload);

  core::EventBus bus;
  UpdateManager upd(&bus, vendor.pkB64(), target, std::move(src));
  upd.setHealthCheck([] { return true; });

  int completed = 0;
  bus.subscribe(core::EventType::UpdateCompleted, [&](const core::Event&) { ++completed; });

  ASSERT_TRUE(upd.runUpdate("http://vendor/update").ok());
  bus.drain();
  EXPECT_EQ(completed, 1);

  // Target now holds the new payload; a backup of the old one exists.
  std::ifstream in(target);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "NEW_BINARY_V2_CONTENTS");
  EXPECT_TRUE(fs::exists(target + ".prev"));
}

TEST(UpdateManager, RejectsTamperedPayload) {
  auto dir = fs::temp_directory_path() / "nexus_upd_tamper";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto target = writeCurrentBinary(dir);

  Vendor vendor;
  auto src = std::make_unique<StubUpdateSource>();
  auto* src_ptr = src.get();
  src_ptr->package = vendor.makePackage("2.0.0", bytesOf("GOOD"));
  src_ptr->package.payload = bytesOf("TAMPERED");  // payload no longer matches the signed hash

  core::EventBus bus;
  UpdateManager upd(&bus, vendor.pkB64(), target, std::move(src));
  upd.setHealthCheck([] { return true; });

  int failed = 0;
  bus.subscribe(core::EventType::UpdateFailed, [&](const core::Event&) { ++failed; });
  EXPECT_FALSE(upd.runUpdate("x").ok());
  bus.drain();
  EXPECT_EQ(failed, 1);
  // The original binary is untouched.
  std::ifstream in(target);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "OLD_BINARY_V1");
}

TEST(UpdateManager, RejectsWrongVendorSignature) {
  auto dir = fs::temp_directory_path() / "nexus_upd_sig";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto target = writeCurrentBinary(dir);

  Vendor real, attacker;
  auto src = std::make_unique<StubUpdateSource>();
  src->package = attacker.makePackage("2.0.0", bytesOf("EVIL"));  // signed by the wrong key

  core::EventBus bus;
  UpdateManager upd(&bus, real.pkB64(), target, std::move(src));  // trusts only `real`
  EXPECT_FALSE(upd.runUpdate("x").ok());
}

TEST(UpdateManager, RollsBackOnHealthCheckFailure) {
  auto dir = fs::temp_directory_path() / "nexus_upd_rollback";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto target = writeCurrentBinary(dir);

  Vendor vendor;
  auto src = std::make_unique<StubUpdateSource>();
  src->package = vendor.makePackage("2.0.0", bytesOf("BROKEN_V2"));

  core::EventBus bus;
  UpdateManager upd(&bus, vendor.pkB64(), target, std::move(src));
  upd.setHealthCheck([] { return false; });  // new binary is unhealthy

  int rolled = 0;
  bus.subscribe(core::EventType::RollbackTriggered, [&](const core::Event&) { ++rolled; });

  EXPECT_FALSE(upd.runUpdate("x").ok());
  bus.drain();
  EXPECT_EQ(rolled, 1);
  // Rolled back to the original binary.
  std::ifstream in(target);
  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "OLD_BINARY_V1");
}

TEST(UpdateManager, RejectsIncompatibleHardware) {
  auto dir = fs::temp_directory_path() / "nexus_upd_hw";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto target = writeCurrentBinary(dir);

  Vendor vendor;
  auto src = std::make_unique<StubUpdateSource>();
  src->package = vendor.makePackage("2.0.0", bytesOf("V2"), /*min_hw*/ "2.0");

  core::EventBus bus;
  UpdateManager upd(&bus, vendor.pkB64(), target, std::move(src));
  EXPECT_FALSE(upd.runUpdate("x", /*current_hw*/ "1.0").ok());
}

TEST(UpdateManager, RefusesWhenUpdatesDisabled) {
  auto dir = fs::temp_directory_path() / "nexus_upd_disabled";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto target = writeCurrentBinary(dir);

  Vendor vendor;
  auto src = std::make_unique<StubUpdateSource>();
  src->package = vendor.makePackage("2.0.0", bytesOf("V2"));

  core::EventBus bus;
  UpdateManager upd(&bus, vendor.pkB64(), target, std::move(src));
  upd.setUpdatesAllowed(false);  // e.g. during calibration
  EXPECT_FALSE(upd.runUpdate("x").ok());
}
