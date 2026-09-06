#include <gtest/gtest.h>

#include <atomic>

#include "core/EventBus.h"
#include "discovery/DiscoveryService.h"

using namespace nexus::discovery;
using nexus::core::Event;
using nexus::core::EventBus;
using nexus::core::EventType;

TEST(DiscoveryService, StartStopLifecycle) {
  EventBus bus;
  DiscoveryService disc(&bus);
  ASSERT_TRUE(disc.start().ok());
  EXPECT_EQ(disc.state(), nexus::core::ServiceState::Running);
  ASSERT_TRUE(disc.stop().ok());
  EXPECT_EQ(disc.state(), nexus::core::ServiceState::Stopped);
}

TEST(DiscoveryService, AdvertiseBeacon) {
  EventBus bus;
  auto hal = std::make_unique<StubDiscoveryHal>();
  auto* hal_ptr = hal.get();
  DiscoveryService disc(&bus, std::move(hal));
  disc.start();

  SpeakerBeacon beacon;
  beacon.device_id = "SPK-ABCDEF01";
  beacon.model = "NEXUS-SPEAKER-3";
  beacon.box_public_key = "boxpub";
  ASSERT_TRUE(disc.advertise(beacon).ok());
  EXPECT_TRUE(hal_ptr->publishing());
  EXPECT_EQ(hal_ptr->publishedBeacon().device_id, "SPK-ABCDEF01");

  ASSERT_TRUE(disc.stopAdvertising().ok());
  EXPECT_FALSE(hal_ptr->publishing());
}

TEST(DiscoveryService, FindStreamerEmitsFoundOnMatch) {
  EventBus bus;
  std::atomic<int> found{0};
  bus.subscribe(EventType::StreamerFound, [&](const Event&) { ++found; });

  DiscoveryService disc(&bus);
  disc.start();
  // StubDiscoveryHal advertises "STR-LAB01".
  auto rec = disc.findStreamer("STR-LAB01");
  bus.drain();
  ASSERT_TRUE(rec.ok());
  EXPECT_EQ(rec.value().host, "streamer.local");
  EXPECT_EQ(found.load(), 1);
}

TEST(DiscoveryService, FindStreamerEmitsLostOnMiss) {
  EventBus bus;
  std::atomic<int> lost{0};
  bus.subscribe(EventType::StreamerLost, [&](const Event&) { ++lost; });

  DiscoveryService disc(&bus);
  disc.start();
  auto rec = disc.findStreamer("STR-NONEXISTENT");
  bus.drain();
  EXPECT_FALSE(rec.ok());
  EXPECT_EQ(lost.load(), 1);
}
