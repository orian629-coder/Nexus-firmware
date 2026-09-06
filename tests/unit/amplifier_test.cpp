#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "amplifier/AmplifierManager.h"
#include "core/EventBus.h"

using namespace nexus::amplifier;
using nexus::core::Event;
using nexus::core::EventBus;
using nexus::core::EventType;

namespace {
// Owns a stub HAL pointer so the test can drive temperature/faults after construction.
struct Rig {
  EventBus bus;
  StubAmplifierHal* hal;
  std::unique_ptr<AmplifierManager> amp;
  explicit Rig(double warn = 70.0, double shutdown = 85.0) {
    auto h = std::make_unique<StubAmplifierHal>();
    hal = h.get();
    amp = std::make_unique<AmplifierManager>(&bus, std::move(h), warn, shutdown);
  }
};
}  // namespace

TEST(AmplifierManager, StartsMutedThenUnmutes) {
  Rig r;
  ASSERT_TRUE(r.amp->start().ok());
  EXPECT_EQ(r.amp->ampState(), AmpState::Starting);
  EXPECT_TRUE(r.hal->muted);

  ASSERT_TRUE(r.amp->unmute().ok());
  EXPECT_EQ(r.amp->ampState(), AmpState::Ready);
  EXPECT_FALSE(r.hal->muted);
}

TEST(AmplifierManager, OverheatMutesAndAlerts) {
  Rig r(70.0, 85.0);
  r.amp->start();
  r.amp->unmute();

  std::atomic<int> overheat{0};
  r.bus.subscribe(EventType::AmplifierOverheat, [&](const Event&) { ++overheat; });

  r.hal->temperature = 90.0;  // above shutdown threshold
  r.amp->poll();
  r.bus.drain();

  EXPECT_EQ(r.amp->ampState(), AmpState::Overheated);
  EXPECT_TRUE(r.hal->muted);
  EXPECT_EQ(overheat.load(), 1);
}

TEST(AmplifierManager, WarningThresholdEmitsTempWarningOnce) {
  Rig r(70.0, 85.0);
  r.amp->start();

  std::atomic<int> warns{0};
  r.bus.subscribe(EventType::TempWarning, [&](const Event&) { ++warns; });

  r.hal->temperature = 75.0;  // warn but not shutdown
  r.amp->poll();
  r.amp->poll();  // still hot — should not re-warn
  r.bus.drain();
  EXPECT_EQ(warns.load(), 1);
}

TEST(AmplifierManager, RecoversAfterCoolDown) {
  Rig r(70.0, 85.0);
  r.amp->start();
  r.hal->temperature = 90.0;
  r.amp->poll();
  ASSERT_EQ(r.amp->ampState(), AmpState::Overheated);

  r.hal->temperature = 50.0;  // cooled below warning
  r.amp->poll();
  EXPECT_EQ(r.amp->ampState(), AmpState::Muted);
}

TEST(AmplifierManager, HardwareFaultTransitionsAndAlerts) {
  Rig r;
  r.amp->start();
  r.amp->unmute();

  std::atomic<int> faults{0};
  r.bus.subscribe(EventType::AmplifierFault, [&](const Event&) { ++faults; });

  r.hal->flags.fault = true;
  r.amp->poll();
  r.bus.drain();

  EXPECT_EQ(r.amp->ampState(), AmpState::Fault);
  EXPECT_TRUE(r.hal->muted);
  EXPECT_EQ(faults.load(), 1);
  EXPECT_FALSE(r.amp->healthCheck().ok());
}

TEST(AmplifierManager, CannotUnmuteWhileFaulted) {
  Rig r;
  r.amp->start();
  r.hal->flags.fault = true;
  r.amp->poll();
  EXPECT_FALSE(r.amp->unmute().ok());
}

TEST(AmplifierManager, ProtectionEmitsEvent) {
  Rig r;
  r.amp->start();
  std::atomic<int> prot{0};
  r.bus.subscribe(EventType::AmplifierProtection, [&](const Event&) { ++prot; });
  r.hal->flags.protection = true;
  r.amp->poll();
  r.bus.drain();
  EXPECT_EQ(r.amp->ampState(), AmpState::Protection);
  EXPECT_EQ(prot.load(), 1);
}
