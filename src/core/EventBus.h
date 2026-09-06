#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Event.h"

namespace nexus::core {

// Thread-safe in-process publish/subscribe bus. Dispatch happens on a single dedicated worker
// thread so publishers never block and event ordering is preserved. Each handler is invoked
// inside a try/catch: a throwing or misbehaving subscriber is isolated and logged, never
// propagated to the publisher or to other subscribers. This decoupling is what lets one module
// fault without taking down the rest.
class EventBus {
 public:
  using Handler = std::function<void(const Event&)>;
  using Token = std::uint64_t;

  EventBus();
  ~EventBus();

  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  // Subscribe to one event type. Returns a token for unsubscribe().
  Token subscribe(EventType type, Handler handler);
  // Subscribe to every event type.
  Token subscribeAll(Handler handler);
  void unsubscribe(Token token);

  // Enqueue an event for asynchronous delivery. Non-blocking; never throws to the caller.
  void publish(Event event);

  // Block until the dispatch queue is drained. Test/shutdown aid.
  void drain();

  // Stop the worker thread (idempotent). Called by the destructor.
  void stop();

 private:
  struct Subscription {
    Token token;
    bool all;
    EventType type;
    Handler handler;
  };

  void workerLoop();
  void dispatch(const Event& event);

  std::mutex subs_mutex_;
  std::vector<Subscription> subs_;
  std::atomic<Token> next_token_{1};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Event> queue_;
  std::atomic<bool> running_{false};
  std::size_t in_flight_ = 0;  // guarded by queue_mutex_; queued + currently dispatching
  std::condition_variable idle_cv_;
  std::thread worker_;
};

}  // namespace nexus::core
