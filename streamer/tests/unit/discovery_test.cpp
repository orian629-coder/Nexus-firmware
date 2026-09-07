#include <gtest/gtest.h>

#include <memory>

#include "discovery/DiscoveryService.h"
#include "discovery/IStreamerDiscovery.h"

using namespace nexus::streamer::discovery;
using nexus::core::ServiceState;

// The stub verifies the interface contract the onboarding flow relies on: advertise records the
// streamer service fields, and browseSpeakers yields a beacon with the fields the PairingClient
// needs (box_public_key + control_port + setup_mode). The real Avahi path is exercised on the Pi.
TEST(StreamerDiscovery, AdvertiseRecordsServiceFields) {
  StubStreamerDiscovery disc;
  EXPECT_FALSE(disc.advertising());

  StreamerAdvertisement ad;
  ad.streamer_id = "STR-1ab01234";
  ad.public_key = "cGstYjY0";
  ad.port = 6789;
  ASSERT_TRUE(disc.advertise(ad).ok());

  EXPECT_TRUE(disc.advertising());
  EXPECT_EQ(disc.advertised().streamer_id, "STR-1ab01234");
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

// Phase 2: DiscoveryService must advertise the streamer as soon as it starts (this is what makes a
// speaker able to find it), publishing the streamer_id/public_key/port, and clear it on stop.
TEST(StreamerDiscoveryService, StartAdvertisesAndStopClears) {
  auto stub = std::make_shared<StubStreamerDiscovery>();
  DiscoveryService svc(stub, "STR-1ab01234", "cGstYjY0", 8090);
  EXPECT_FALSE(stub->advertising());

  ASSERT_TRUE(svc.start().ok());
  EXPECT_EQ(svc.state(), ServiceState::Running);
  EXPECT_TRUE(stub->advertising());
  EXPECT_EQ(stub->advertised().streamer_id, "STR-1ab01234");
  EXPECT_EQ(stub->advertised().public_key, "cGstYjY0");
  EXPECT_EQ(stub->advertised().port, 8090);

  ASSERT_TRUE(svc.stop().ok());
  EXPECT_EQ(svc.state(), ServiceState::Stopped);
  EXPECT_FALSE(stub->advertising());
}

// A build with no mDNS backend (null discovery) must not fail startup — the streamer still runs and
// speakers can be added by address; the service reports Degraded rather than erroring.
TEST(StreamerDiscoveryService, NullDiscoveryStartsDegraded) {
  DiscoveryService svc(nullptr, "STR-1ab01234", "cGstYjY0", 8090);
  ASSERT_TRUE(svc.start().ok());
  EXPECT_EQ(svc.state(), ServiceState::Degraded);
}
