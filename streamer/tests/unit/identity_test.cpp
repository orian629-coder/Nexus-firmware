#include <gtest/gtest.h>

#include <sys/stat.h>

#include <filesystem>

#include "identity/Crypto.h"
#include "identity/StreamerIdentity.h"

namespace fs = std::filesystem;
using nexus::streamer::identity::StreamerIdentity;

namespace {
fs::path tmpKey(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_id_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d / "identity.key";
}
}  // namespace

// First load generates a key; a second load of the same path reuses it (same public key + id).
TEST(StreamerIdentity, PersistsAndReloadsSameKey) {
  const auto path = tmpKey("persist").string();

  StreamerIdentity a(path);
  ASSERT_TRUE(a.load().ok());
  EXPECT_FALSE(a.publicKeyBase64().empty());
  EXPECT_EQ(a.streamerId().rfind("STR-", 0), 0u);

  StreamerIdentity b(path);
  ASSERT_TRUE(b.load().ok());
  EXPECT_EQ(a.publicKeyBase64(), b.publicKeyBase64());
  EXPECT_EQ(a.secretKeyBase64(), b.secretKeyBase64());
  EXPECT_EQ(a.streamerId(), b.streamerId());
}

// The persisted key file is owner-only (0600).
TEST(StreamerIdentity, KeyFileIsOwnerOnly) {
  const auto path = tmpKey("perms").string();
  StreamerIdentity id(path);
  ASSERT_TRUE(id.load().ok());

  struct stat st{};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

// The secret key signs and the derived public key verifies — the identity is internally consistent.
TEST(StreamerIdentity, SecretAndPublicKeyAgree) {
  const auto path = tmpKey("agree").string();
  StreamerIdentity id(path);
  ASSERT_TRUE(id.load().ok());

  const std::string msg = "cmd-1\nSPK-A\nSET_VOLUME\n{\"volume\":10}\n100\n50";
  auto sig = nexus::identity::crypto::signEd25519Base64(msg, id.secretKeyBase64());
  ASSERT_TRUE(sig.ok());
  EXPECT_TRUE(nexus::identity::crypto::verifyEd25519Base64(
      nexus::identity::crypto::toBase64(std::vector<std::uint8_t>(msg.begin(), msg.end())),
      sig.value(), id.publicKeyBase64()));
}
