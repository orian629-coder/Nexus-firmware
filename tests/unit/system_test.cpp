#include <gtest/gtest.h>

#include <atomic>

#include <memory>

#include "core/EventBus.h"
#include "core/StubService.h"
#include "system/SafeMode.h"
#include "system/StateMachine.h"
#include "system/SystemManager.h"
#include "system/Watchdog.h"

using namespace nexus::system;
using nexus::core::EventBus;
using nexus::core::Event;
using nexus::core::EventType;

TEST(StateMachine, StartsInBooting) {
  StateMachine sm;
  EXPECT_EQ(sm.current(), SystemState::Booting);
}

TEST(StateMachine, AllowsLegalTransition) {
  StateMachine sm;
  ASSERT_TRUE(sm.transitionTo(SystemState::Unconfigured, "boot done").ok());
  EXPECT_EQ(sm.current(), SystemState::Unconfigured);
  ASSERT_TRUE(sm.transitionTo(SystemState::SetupMode, "need setup").ok());
  EXPECT_EQ(sm.current(), SystemState::SetupMode);
}

TEST(StateMachine, RejectsIllegalTransition) {
  StateMachine sm;
  // Booting cannot jump straight to Playing.
  auto s = sm.transitionTo(SystemState::Playing, "nope");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), nexus::core::ErrorCode::IllegalStateTransition);
  EXPECT_EQ(sm.current(), SystemState::Booting);  // unchanged
}

TEST(StateMachine, ShutdownAlwaysLegal) {
  StateMachine sm;
  ASSERT_TRUE(sm.transitionTo(SystemState::ConnectingNetwork, "x").ok());
  EXPECT_TRUE(sm.transitionTo(SystemState::ShuttingDown, "power off").ok());
}

TEST(StateMachine, ErrorPathAlwaysLegal) {
  StateMachine sm;
  ASSERT_TRUE(sm.transitionTo(SystemState::ConnectingNetwork, "x").ok());
  EXPECT_TRUE(sm.transitionTo(SystemState::Error, "fault").ok());
  // Error can recover to Booting.
  EXPECT_TRUE(sm.transitionTo(SystemState::Booting, "recover").ok());
}

TEST(StateMachine, EmitsStateChangedEvent) {
  EventBus bus;
  std::atomic<int> changes{0};
  bus.subscribe(EventType::StateChanged, [&](const Event&) { ++changes; });
  StateMachine sm(&bus);
  ASSERT_TRUE(sm.transitionTo(SystemState::Unconfigured, "x").ok());
  bus.drain();
  EXPECT_EQ(changes.load(), 1);
}

TEST(StateMachine, OnEnterHookFires) {
  StateMachine sm;
  std::atomic<bool> fired{false};
  sm.onEnter(SystemState::Unconfigured, [&](SystemState, SystemState) { fired = true; });
  sm.transitionTo(SystemState::Unconfigured, "x");
  EXPECT_TRUE(fired.load());
}

TEST(SystemManager, DrivesTransitionsFromEvents) {
  EventBus bus;
  SystemManager mgr(&bus);
  ASSERT_TRUE(mgr.start().ok());
  mgr.enterInitialState(/*configured=*/true);  // Booting -> ConnectingNetwork
  EXPECT_EQ(mgr.states().current(), SystemState::ConnectingNetwork);

  bus.publish(Event{EventType::NetworkConnected, "network"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::SearchingStreamer);

  bus.publish(Event{EventType::StreamerFound, "discovery"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::Authenticating);

  bus.publish(Event{EventType::PairingCompleted, "pairing"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::Online);

  mgr.stop();
}

// Observed on a real Pi: the speaker was playing audio while /api/status still reported
// SEARCHING_STREAMER, with "illegal transition SEARCHING_STREAMER -> PLAYING" in the journal.
//
// The UDP audio receiver needs no handshake and is always listening, so a streamer can start
// sending before it has been discovered over mDNS (or without advertising at all). Audio arriving
// is itself proof that a streamer is there, so it must be able to reach Playing — otherwise every
// consumer of the state field, the streamer's monitor included, is told the speaker is idle while
// sound is coming out of it.
TEST(SystemManager, AudioWhileSearchingStreamerStillReachesPlaying) {
  EventBus bus;
  SystemManager mgr(&bus);
  ASSERT_TRUE(mgr.start().ok());
  mgr.enterInitialState(/*configured=*/true);
  bus.publish(Event{EventType::NetworkConnected, "network"});
  bus.drain();
  ASSERT_EQ(mgr.states().current(), SystemState::SearchingStreamer);

  // No StreamerFound / PairingCompleted — audio simply starts arriving.
  bus.publish(Event{EventType::AudioStarted, "audio"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::Playing)
      << "playing audio must be reported as PLAYING, not SEARCHING_STREAMER";

  bus.publish(Event{EventType::AudioStopped, "audio"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::Online);

  mgr.stop();
}

// The connectivity monitor republishes NetworkConnected every few seconds so downstream consumers
// can re-assert derived state. Also observed on the Pi: that poll dragged a PLAYING speaker back to
// SEARCHING_STREAMER on every tick. The same bug reset AUTHENTICATING each tick, so a handshake
// slower than the poll interval could never complete.
TEST(SystemManager, RepeatedNetworkConnectedDoesNotDisturbAnEstablishedSession) {
  EventBus bus;
  SystemManager mgr(&bus);
  ASSERT_TRUE(mgr.start().ok());
  mgr.enterInitialState(/*configured=*/true);
  bus.publish(Event{EventType::NetworkConnected, "network"});
  bus.drain();
  bus.publish(Event{EventType::StreamerFound, "discovery"});
  bus.drain();
  ASSERT_EQ(mgr.states().current(), SystemState::Authenticating);

  // A poll tick mid-handshake must not restart discovery.
  bus.publish(Event{EventType::NetworkConnected, "network"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::Authenticating)
      << "the poll reset an in-flight handshake";

  bus.publish(Event{EventType::PairingCompleted, "pairing"});
  bus.drain();
  bus.publish(Event{EventType::AudioStarted, "audio"});
  bus.drain();
  ASSERT_EQ(mgr.states().current(), SystemState::Playing);

  // ...and must not interrupt playback.
  for (int i = 0; i < 3; ++i) {
    bus.publish(Event{EventType::NetworkConnected, "network"});
    bus.drain();
  }
  EXPECT_EQ(mgr.states().current(), SystemState::Playing)
      << "the connectivity poll knocked the speaker out of PLAYING";

  mgr.stop();
}

TEST(SystemManager, ConfigInvalidGoesDegraded) {
  EventBus bus;
  SystemManager mgr(&bus);
  ASSERT_TRUE(mgr.start().ok());
  mgr.enterInitialState(true);
  // Drive to Online first (Degraded is only reachable from Online/Playing/etc).
  bus.publish(Event{EventType::NetworkConnected, "n"});
  bus.publish(Event{EventType::StreamerFound, "d"});
  bus.publish(Event{EventType::PairingCompleted, "p"});
  bus.drain();
  ASSERT_EQ(mgr.states().current(), SystemState::Online);

  bus.publish(Event{EventType::ConfigInvalid, "config"});
  bus.drain();
  EXPECT_EQ(mgr.states().current(), SystemState::Degraded);
  mgr.stop();
}

// ── Watchdog ──

namespace {
// A service whose health can be toggled for watchdog tests.
class FlakyService : public nexus::core::IService {
 public:
  std::string name() const override { return "flaky"; }
  nexus::core::Status start() override { return {}; }
  nexus::core::Status stop() override { return {}; }
  nexus::core::ServiceState state() const override { return nexus::core::ServiceState::Running; }
  nexus::core::Status healthCheck() override {
    return healthy ? nexus::core::Status::success()
                   : nexus::core::Status::error(nexus::core::ErrorCode::Unknown, "sick");
  }
  bool healthy = true;
};
}  // namespace

TEST(Watchdog, HealthyServiceNoRecovery) {
  EventBus bus;
  Watchdog wd(&bus, 5000, 3);
  FlakyService svc;
  wd.watch(&svc);
  int recoveries = 0;
  wd.setRecoveryHandler([&](const std::string&) { ++recoveries; });
  wd.pollOnce();
  wd.pollOnce();
  wd.pollOnce();
  EXPECT_EQ(recoveries, 0);
}

TEST(Watchdog, FiresRecoveryAfterThreshold) {
  EventBus bus;
  Watchdog wd(&bus, 5000, 3);
  FlakyService svc;
  svc.healthy = false;
  wd.watch(&svc);

  std::atomic<int> health_fail{0}, timeout{0};
  bus.subscribe(EventType::HealthCheckFailed, [&](const Event&) { ++health_fail; });
  bus.subscribe(EventType::WatchdogTimeout, [&](const Event&) { ++timeout; });
  int recoveries = 0;
  wd.setRecoveryHandler([&](const std::string& s) {
    ++recoveries;
    EXPECT_EQ(s, "flaky");
  });

  wd.pollOnce();  // 1
  wd.pollOnce();  // 2
  wd.pollOnce();  // 3 → recovery
  bus.drain();
  EXPECT_EQ(recoveries, 1);
  EXPECT_EQ(timeout.load(), 1);
  EXPECT_GE(health_fail.load(), 3);
}

TEST(Watchdog, RecoveringResetsFailCount) {
  EventBus bus;
  Watchdog wd(&bus, 5000, 3);
  FlakyService svc;
  wd.watch(&svc);
  int recoveries = 0;
  wd.setRecoveryHandler([&](const std::string&) { ++recoveries; });

  svc.healthy = false;
  wd.pollOnce();
  wd.pollOnce();
  svc.healthy = true;   // recovered before threshold
  wd.pollOnce();
  svc.healthy = false;  // starts counting again from 0
  wd.pollOnce();
  wd.pollOnce();
  EXPECT_EQ(recoveries, 0);  // never reached 3 consecutive
}

// ── SafeMode ──

TEST(SafeMode, EnterRunsDisableActionOnce) {
  EventBus bus;
  SafeMode sm(&bus);
  int disabled = 0;
  sm.setDisableAction([&] { ++disabled; });
  std::atomic<int> entered{0};
  bus.subscribe(EventType::SafeModeEntered, [&](const Event&) { ++entered; });

  EXPECT_FALSE(sm.active());
  sm.enter("test");
  sm.enter("again");  // idempotent
  bus.drain();
  EXPECT_TRUE(sm.active());
  EXPECT_EQ(disabled, 1);
  EXPECT_EQ(entered.load(), 1);
}

TEST(SystemManager, ConfigInvalidEntersSafeMode) {
  EventBus bus;
  SystemManager mgr(&bus);
  int disabled = 0;
  mgr.setSafeModeDisableAction([&] { ++disabled; });
  mgr.start();
  mgr.enterInitialState(true);

  std::atomic<int> safe{0};
  bus.subscribe(EventType::SafeModeEntered, [&](const Event&) { ++safe; });
  bus.publish(Event{EventType::ConfigInvalid, "config"});
  bus.drain();

  EXPECT_TRUE(mgr.safeMode().active());
  EXPECT_EQ(disabled, 1);
  EXPECT_EQ(safe.load(), 1);
  mgr.stop();
}
