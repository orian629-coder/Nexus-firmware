#include <gtest/gtest.h>

#include "identity/ApCredentials.h"

// Contract guard: the streamer hosts the AP each speaker joins, so it MUST derive from the same
// shared module. This proves the module links in the streamer tree and yields a WPA2-valid,
// stable SSID/passphrase for a known streamer_id.
TEST(StreamerApCredentialsContract, DerivesValidStableCredentials) {
  const auto c = nexus::identity::deriveApCredentials("STR-LAB01");
  EXPECT_EQ(c.ssid, "Nexus-STR-LAB01");
  EXPECT_EQ(c.passphrase, nexus::identity::deriveApCredentials("STR-LAB01").passphrase);
  ASSERT_GE(c.passphrase.size(), 8u);
  ASSERT_LE(c.passphrase.size(), 63u);
}
