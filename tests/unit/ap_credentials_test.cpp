#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "identity/ApCredentials.h"

using nexus::identity::ApCredentials;
using nexus::identity::deriveApCredentials;
using nexus::identity::streamerIdFromApSsid;

TEST(ApCredentials, SsidIsNexusPrefixPlusStreamerId) {
  const ApCredentials c = deriveApCredentials("STR-a14ad83e");
  EXPECT_EQ(c.ssid, "Nexus-STR-a14ad83e");
}

TEST(ApCredentials, DerivationIsDeterministic) {
  EXPECT_EQ(deriveApCredentials("STR-a14ad83e").passphrase,
            deriveApCredentials("STR-a14ad83e").passphrase);
}

TEST(ApCredentials, DifferentStreamersGetDifferentPassphrases) {
  EXPECT_NE(deriveApCredentials("STR-a14ad83e").passphrase,
            deriveApCredentials("STR-deadbeef").passphrase);
}

TEST(ApCredentials, PassphraseIsWpa2ValidLengthAndCharset) {
  const std::string pass = deriveApCredentials("STR-a14ad83e").passphrase;
  ASSERT_GE(pass.size(), 8u);
  ASSERT_LE(pass.size(), 63u);
  for (unsigned char ch : pass) {
    EXPECT_TRUE(std::isprint(ch)) << "non-printable char in passphrase";
  }
}

TEST(ApCredentials, StreamerIdRoundTripsFromSsid) {
  const ApCredentials c = deriveApCredentials("STR-a14ad83e");
  const auto id = streamerIdFromApSsid(c.ssid);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, "STR-a14ad83e");
}

TEST(ApCredentials, SetupApAndForeignSsidsAreNotStreamerAps) {
  EXPECT_FALSE(streamerIdFromApSsid("Nexus-Setup").has_value());
  EXPECT_FALSE(streamerIdFromApSsid("HomeWiFi").has_value());
  EXPECT_FALSE(streamerIdFromApSsid("").has_value());
}

TEST(ApCredentials, IsWellFormedStreamerId) {
  using nexus::identity::isWellFormedStreamerId;
  EXPECT_TRUE(isWellFormedStreamerId("STR-a14ad83e"));
  EXPECT_FALSE(isWellFormedStreamerId(""));
  EXPECT_FALSE(isWellFormedStreamerId("STR-A14AD83E"));      // uppercase
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad83"));       // 7 hex
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad83ee"));     // 9 hex
  EXPECT_FALSE(isWellFormedStreamerId("SPK-a14ad83e"));      // wrong prefix
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad8;e"));      // metachar
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad83e\n$(x)")); // injection
}
