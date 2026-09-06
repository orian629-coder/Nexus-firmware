#include <gtest/gtest.h>

#include <atomic>
#include <string>

#include "core/EventBus.h"
#include "core/Result.h"
#include "core/SecretString.h"
#include "core/StubService.h"

using namespace nexus::core;

TEST(Result, StatusOkAndError) {
  Status ok;
  EXPECT_TRUE(ok.ok());
  EXPECT_EQ(ok.code(), ErrorCode::Ok);

  Status err = Status::error(ErrorCode::NotFound, "missing");
  EXPECT_FALSE(err.ok());
  EXPECT_EQ(err.code(), ErrorCode::NotFound);
  EXPECT_EQ(err.message(), "missing");
}

TEST(Result, HoldsValueOrError) {
  Result<int> good(42);
  EXPECT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);

  Result<int> bad(Status::error(ErrorCode::IoError, "boom"));
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.code(), ErrorCode::IoError);
  EXPECT_EQ(bad.value_or(-1), -1);
}

TEST(SecretString, RedactsOnStream) {
  SecretString s("hunter2");
  std::ostringstream os;
  os << s;
  EXPECT_EQ(os.str(), "***");
  EXPECT_EQ(s.reveal(), "hunter2");  // explicit access still works
}

TEST(StubService, StartStopTogglesState) {
  EventBus bus;
  NEXUS_STUB_SERVICE(FakeService, "fake");
  FakeService svc(&bus);
  EXPECT_EQ(svc.state(), ServiceState::Stopped);
  EXPECT_TRUE(svc.start().ok());
  EXPECT_EQ(svc.state(), ServiceState::Running);
  EXPECT_TRUE(svc.stop().ok());
  EXPECT_EQ(svc.state(), ServiceState::Stopped);
  EXPECT_EQ(svc.name(), "fake");
}

TEST(EventBus, DeliversToSubscribers) {
  EventBus bus;
  std::atomic<int> count{0};
  bus.subscribe(EventType::NetworkConnected, [&](const Event&) { ++count; });
  bus.publish(Event{EventType::NetworkConnected, "test"});
  bus.publish(Event{EventType::NetworkDisconnected, "test"});  // different type, ignored
  bus.drain();
  EXPECT_EQ(count.load(), 1);
}

TEST(EventBus, SubscribeAllReceivesEverything) {
  EventBus bus;
  std::atomic<int> count{0};
  bus.subscribeAll([&](const Event&) { ++count; });
  bus.publish(Event{EventType::NetworkConnected, "t"});
  bus.publish(Event{EventType::StateChanged, "t"});
  bus.drain();
  EXPECT_EQ(count.load(), 2);
}

TEST(EventBus, IsolatesThrowingSubscriber) {
  EventBus bus;
  std::atomic<int> good{0};
  bus.subscribe(EventType::StateChanged, [](const Event&) { throw std::runtime_error("bad"); });
  bus.subscribe(EventType::StateChanged, [&](const Event&) { ++good; });
  bus.publish(Event{EventType::StateChanged, "t"});
  bus.drain();
  // The throwing subscriber must not prevent the healthy one from running.
  EXPECT_EQ(good.load(), 1);
}

TEST(EventBus, UnsubscribeStopsDelivery) {
  EventBus bus;
  std::atomic<int> count{0};
  auto tok = bus.subscribe(EventType::StateChanged, [&](const Event&) { ++count; });
  bus.unsubscribe(tok);
  bus.publish(Event{EventType::StateChanged, "t"});
  bus.drain();
  EXPECT_EQ(count.load(), 0);
}
