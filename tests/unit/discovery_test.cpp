#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

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
  // StubDiscoveryHal advertises "STR-1ab01234".
  auto rec = disc.findStreamer("STR-1ab01234");
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

// The key StubDiscoveryHal advertises for STR-1ab01234 (see IDiscoveryHal.h).
namespace {
constexpr const char* kLabStreamerKey = "c3RyZWFtZXItcHVibGljLWtleQ==";

// Poll a predicate up to `budget`, so async-worker tests don't sleep for a fixed slab of time.
template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds budget = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}
}  // namespace

// Trust check: a streamer_id match with the WRONG public key is a possible spoof — it must not be
// accepted, and StreamerFound must not fire.
TEST(DiscoveryService, FindStreamerRejectsPublicKeyMismatch) {
  EventBus bus;
  std::atomic<int> found{0};
  std::atomic<int> lost{0};
  bus.subscribe(EventType::StreamerFound, [&](const Event&) { ++found; });
  bus.subscribe(EventType::StreamerLost, [&](const Event&) { ++lost; });

  DiscoveryService disc(&bus);
  disc.start();
  auto rec = disc.findStreamer("STR-1ab01234", "not-the-real-key");
  bus.drain();
  EXPECT_FALSE(rec.ok());
  EXPECT_EQ(found.load(), 0);
  EXPECT_EQ(lost.load(), 1);
}

// Trust check: id AND public key match → accepted.
TEST(DiscoveryService, FindStreamerAcceptsMatchingPublicKey) {
  EventBus bus;
  std::atomic<int> found{0};
  bus.subscribe(EventType::StreamerFound, [&](const Event&) { ++found; });

  DiscoveryService disc(&bus);
  disc.start();
  auto rec = disc.findStreamer("STR-1ab01234", kLabStreamerKey);
  bus.drain();
  ASSERT_TRUE(rec.ok());
  EXPECT_EQ(rec.value().public_key, kLabStreamerKey);
  EXPECT_EQ(found.load(), 1);
}

// The browse worker: startSearching keeps browsing until it finds the target, emits StreamerFound
// exactly once, then self-stops.
TEST(DiscoveryService, StartSearchingFindsStreamerThenStops) {
  EventBus bus;
  std::atomic<int> found{0};
  bus.subscribe(EventType::StreamerFound, [&](const Event&) { ++found; });

  DiscoveryService disc(&bus);
  disc.start();
  disc.startSearching("STR-1ab01234", kLabStreamerKey, std::chrono::milliseconds(10));

  ASSERT_TRUE(waitUntil([&] { return found.load() >= 1; }));
  EXPECT_TRUE(waitUntil([&] { return !disc.searching(); }));  // self-stopped after the match
  std::this_thread::sleep_for(std::chrono::milliseconds(40));  // no further polls should re-emit
  bus.drain();
  EXPECT_EQ(found.load(), 1);

  disc.stopSearching();
}

// The browse worker keeps looking (does not emit StreamerFound, does not spam StreamerLost) while
// the target streamer is not present, and stops cleanly on request.
TEST(DiscoveryService, StartSearchingIgnoresNonMatchingStreamerUntilStopped) {
  EventBus bus;
  std::atomic<int> found{0};
  std::atomic<int> lost{0};
  bus.subscribe(EventType::StreamerFound, [&](const Event&) { ++found; });
  bus.subscribe(EventType::StreamerLost, [&](const Event&) { ++lost; });

  DiscoveryService disc(&bus);
  disc.start();
  disc.startSearching("STR-NOT-ON-THE-LAN", "", std::chrono::milliseconds(5));

  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_TRUE(disc.searching());  // still looking
  bus.drain();
  EXPECT_EQ(found.load(), 0);
  EXPECT_EQ(lost.load(), 0);  // silent retry while searching, not a StreamerLost storm

  disc.stopSearching();
  EXPECT_FALSE(disc.searching());
}
