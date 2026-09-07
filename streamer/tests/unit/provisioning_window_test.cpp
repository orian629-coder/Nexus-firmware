#include "provisioning/ProvisioningWindow.h"
#include <gtest/gtest.h>
using nexus::streamer::provisioning::ProvisioningWindow;

TEST(ProvisioningWindow, DefaultClosed) {
  ProvisioningWindow w;
  EXPECT_FALSE(w.isOpen(1000));
  EXPECT_EQ(w.secondsRemaining(1000), 0);
}
TEST(ProvisioningWindow, OpenThenExpires) {
  ProvisioningWindow w;
  w.open(/*now=*/1000, /*ttl=*/600);
  EXPECT_TRUE(w.isOpen(1000));
  EXPECT_TRUE(w.isOpen(1599));
  EXPECT_EQ(w.secondsRemaining(1000), 600);
  EXPECT_FALSE(w.isOpen(1600));            // expired at boundary
  EXPECT_EQ(w.secondsRemaining(1600), 0);
}
TEST(ProvisioningWindow, ManualClose) {
  ProvisioningWindow w;
  w.open(1000, 600);
  w.close();
  EXPECT_FALSE(w.isOpen(1000));
}
TEST(ProvisioningWindow, TtlClamped) {
  ProvisioningWindow w;
  w.open(0, 100000);                        // over max
  EXPECT_TRUE(w.isOpen(1799));
  EXPECT_FALSE(w.isOpen(1800));             // clamped to 1800
  w.open(0, -5);                            // under min
  EXPECT_TRUE(w.isOpen(0));
  EXPECT_FALSE(w.isOpen(1));
}
