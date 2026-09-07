// Contract test for the speaker-side `--ap-credentials` derivation.
//
// A paired speaker must derive the SAME private-AP SSID + passphrase as the streamer that hosts
// the AP, from the paired streamer_id stored in its config. This anchors scripts/speaker-ap-join.sh
// (which shells out to `nexus-speaker --ap-credentials`) to the one KDF in nexus::identity, so the
// frozen contract (salt + length) lives in exactly one place.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "identity/ApCredentials.h"
#include "main/ApCredentialsCli.h"

namespace {

std::string writeConfig(const std::string& name, const std::string& body) {
  const std::string path = ::testing::TempDir() + name;
  std::ofstream(path) << body;
  return path;
}

}  // namespace

TEST(SpeakerApCredentials, PairedConfigYieldsStreamerDerivedCreds) {
  const std::string path = writeConfig(
      "nexus-apcli-paired.json",
      R"({"pairing":{"paired":true,"streamer_id":"STR-a14ad83e","streamer_public_key":"x"}})");

  const auto creds = nexus::app::apCredentialsFromConfig(path);
  ASSERT_TRUE(creds.has_value());

  // Must match the streamer's own derivation byte-for-byte (single source of truth).
  const auto expected = nexus::identity::deriveApCredentials("STR-a14ad83e");
  EXPECT_EQ(creds->ssid, expected.ssid);
  EXPECT_EQ(creds->passphrase, expected.passphrase);
  EXPECT_EQ(creds->ssid, "Nexus-STR-a14ad83e");

  std::remove(path.c_str());
}

TEST(SpeakerApCredentials, UnpairedReturnsNullopt) {
  const std::string path =
      writeConfig("nexus-apcli-unpaired.json", R"({"pairing":{"paired":false,"streamer_id":""}})");
  EXPECT_FALSE(nexus::app::apCredentialsFromConfig(path).has_value());
  std::remove(path.c_str());
}

TEST(SpeakerApCredentials, PairedButEmptyStreamerIdReturnsNullopt) {
  const std::string path = writeConfig("nexus-apcli-emptyid.json",
                                       R"({"pairing":{"paired":true,"streamer_id":""}})");
  EXPECT_FALSE(nexus::app::apCredentialsFromConfig(path).has_value());
  std::remove(path.c_str());
}

TEST(SpeakerApCredentials, MissingFileReturnsNullopt) {
  EXPECT_FALSE(
      nexus::app::apCredentialsFromConfig("/nonexistent/nexus/does-not-exist.json").has_value());
}

TEST(SpeakerApCredentials, GarbageJsonReturnsNullopt) {
  const std::string path = writeConfig("nexus-apcli-garbage.json", "not json at all {");
  EXPECT_FALSE(nexus::app::apCredentialsFromConfig(path).has_value());
  std::remove(path.c_str());
}

// Security: streamer_id is peer-supplied at pairing time. A value that is not the trusted
// "STR-"+8-hex shape must yield NO credentials, so shell consumers never see an injectable SSID.
TEST(SpeakerApCredentials, MalformedStreamerIdWithShellMetacharsReturnsNullopt) {
  const std::string path = writeConfig(
      "nexus-apcli-inject.json",
      "{\"pairing\":{\"paired\":true,\"streamer_id\":\"STR-x\\n$(touch /tmp/pwn)\\n#\"}}");
  EXPECT_FALSE(nexus::app::apCredentialsFromConfig(path).has_value());
  std::remove(path.c_str());
}

TEST(SpeakerApCredentials, WrongShapeStreamerIdReturnsNullopt) {
  for (const char* id : {"STR-ABCDEF12",       // uppercase hex not allowed
                         "STR-zzzzzzzz",        // non-hex
                         "STR-a14ad83",         // 7 chars
                         "STR-a14ad83ee",       // 9 chars
                         "SPK-a14ad83e",        // wrong prefix
                         "a14ad83e"}) {         // no prefix
    const std::string path = writeConfig(
        "nexus-apcli-shape.json",
        std::string("{\"pairing\":{\"paired\":true,\"streamer_id\":\"") + id + "\"}}");
    EXPECT_FALSE(nexus::app::apCredentialsFromConfig(path).has_value()) << "id=" << id;
    std::remove(path.c_str());
  }
}
