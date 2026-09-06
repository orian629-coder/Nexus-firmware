#include <gtest/gtest.h>

#include "discovery/IStreamerDiscovery.h"

using namespace nexus::streamer::discovery;

// The stub verifies the interface contract the onboarding flow relies on: advertise records the
// streamer service fields, and browseSpeakers yields a beacon with the fields the PairingClient
// needs (box_public_key + control_port + setup_mode). The real Avahi path is exercised on the Pi.
TEST(StreamerDiscovery, AdvertiseRecordsServiceFields) {
  StubStreamerDiscovery disc;
  EXPECT_FALSE(disc.advertising());

  StreamerAdvertisement ad;
  ad.streamer_id = "STR-LAB01";
  ad.public_key = "cGstYjY0";
  ad.port = 6789;
  ASSERT_TRUE(disc.advertise(ad).ok());

  EXPECT_TRUE(disc.advertising());
  EXPECT_EQ(disc.advertised().streamer_id, "STR-LAB01");
  EXPECT_EQ(disc.advertised().public_key, "cGstYjY0");

  ASSERT_TRUE(disc.stopAdvertising().ok());
  EXPECT_FALSE(disc.advertising());
}

TEST(StreamerDiscovery, BrowseYieldsPairableSpeakerBeacon) {
  StubStreamerDiscovery disc;
  auto res = disc.browseSpeakers();
  ASSERT_TRUE(res.ok());
  ASSERT_EQ(res.value().size(), 1u);
  const auto& b = res.value().front();
  EXPECT_FALSE(b.device_id.empty());
  EXPECT_FALSE(b.host.empty());             // resolved IP for pairing/control
  EXPECT_FALSE(b.box_public_key.empty());   // needed to seal Wi-Fi creds
  EXPECT_EQ(b.control_port, 45455);
  EXPECT_TRUE(b.setup_mode);
}
