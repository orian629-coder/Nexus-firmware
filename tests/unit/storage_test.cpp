#include <gtest/gtest.h>

#include <filesystem>
#include <sys/stat.h>

#include "storage/SecureStorage.h"

using namespace nexus::storage;
using nexus::core::SecretString;
namespace fs = std::filesystem;

namespace {
std::string freshDir() {
  auto d = fs::temp_directory_path() / "nexus_secure_test";
  fs::remove_all(d);
  return d.string();
}
}  // namespace

TEST(SecureStorage, PutGetRoundTrip) {
  SecureStorage s(freshDir());
  ASSERT_TRUE(s.put("wifi_psk", SecretString("s3cr3t")).ok());
  ASSERT_TRUE(s.has("wifi_psk"));
  auto r = s.get("wifi_psk");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().reveal(), "s3cr3t");
}

TEST(SecureStorage, FilesAre0600) {
  auto dir = freshDir();
  SecureStorage s(dir);
  ASSERT_TRUE(s.put("k", SecretString("v")).ok());
  struct stat st{};
  ASSERT_EQ(::stat((dir + "/k").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST(SecureStorage, RejectsPathTraversalKeys) {
  SecureStorage s(freshDir());
  EXPECT_FALSE(s.put("../escape", SecretString("x")).ok());
  EXPECT_FALSE(s.put("a/b", SecretString("x")).ok());
  EXPECT_FALSE(s.get("../../etc/passwd").ok());
}

TEST(SecureStorage, EraseRemoves) {
  SecureStorage s(freshDir());
  ASSERT_TRUE(s.put("k", SecretString("v")).ok());
  ASSERT_TRUE(s.erase("k").ok());
  EXPECT_FALSE(s.has("k"));
}
