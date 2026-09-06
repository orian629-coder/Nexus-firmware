#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "app/CommandGateway.h"
#include "core/IService.h"
#include "core/Result.h"
#include "group/SpeakerRegistry.h"
#include "state/SpeakerStateStore.h"

namespace nexus::streamer::state {

// Polls every registered speaker for its real state.
//
// The speaker's StatusService builds heartbeats but is constructed with no sink, so they are
// published to its own in-process EventBus and never leave the device. Until that changes, the only
// way to learn a speaker's state is to ask: a signed GET_STATUS over the control channel. That also
// gives reachability for free — a speaker that stops answering is detected here rather than staying
// green until someone happens to click something.
class MonitorService : public core::IService {
 public:
  // `store` is not written here (see pollSpeaker) but is held so the service owns a coherent view
  // of what it is monitoring; the offline threshold lives with the gateway's reply observer, which
  // is the single writer.
  MonitorService(app::CommandGateway& gateway, group::SpeakerRegistry& registry,
                 SpeakerStateStore& store, int poll_interval_ms = 5000)
      : gateway_(gateway),
        registry_(registry),
        store_(store),
        poll_interval_ms_(poll_interval_ms) {}

  ~MonitorService() override { stop(); }

  std::string name() const override { return "monitor"; }

  core::Status start() override {
    if (running_.exchange(true)) return core::Status::success();
    stopping_ = false;
    state_ = core::ServiceState::Running;
    thread_ = std::thread([this] { loop(); });
    return core::Status::success();
  }

  core::Status stop() override {
    stopping_ = true;
    if (!running_.exchange(false)) {
      stopping_ = false;
      return core::Status::success();
    }
    state_ = core::ServiceState::Stopping;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    stopping_ = false;
    state_ = core::ServiceState::Stopped;
    return core::Status::success();
  }

  core::ServiceState state() const override { return state_; }

  // Poll every speaker once, synchronously. Exposed so tests can drive exact ticks instead of
  // sleeping, and so a UI refresh can force an immediate update.
  //
  // Callable whether or not the service thread is running; `stopping_` (set only by stop()) is what
  // aborts a sweep in progress, so a direct call is never silently skipped.
  void pollOnce() {
    for (const auto& sp : registry_.list()) {
      if (stopping_.load()) return;  // stop() came in mid-sweep
      pollSpeaker(sp);
    }
    ticks_.fetch_add(1);
  }

  std::uint64_t ticks() const { return ticks_.load(); }

 private:
  // Ask the speaker for its state. Recording the OUTCOME is deliberately not done here: the gateway
  // funnels every exchange into the store through its reply observer, so a user's SET_VOLUME and a
  // background poll are recorded by exactly the same code path. Applying it here as well would
  // count each failed poll twice and drop a speaker offline in half the configured attempts.
  void pollSpeaker(const group::Speaker& sp) {
    gateway_.send(sp, "GET_STATUS", nlohmann::json::object());
  }

  void loop() {
    while (running_.load()) {
      pollOnce();
      std::unique_lock<std::mutex> lk(sleep_mutex_);
      cv_.wait_for(lk, std::chrono::milliseconds(poll_interval_ms_),
                   [this] { return !running_.load(); });
    }
  }

  app::CommandGateway& gateway_;
  group::SpeakerRegistry& registry_;
  SpeakerStateStore& store_;
  int poll_interval_ms_;

  std::atomic<core::ServiceState> state_{core::ServiceState::Stopped};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> ticks_{0};
  std::thread thread_;
  std::mutex sleep_mutex_;
  std::condition_variable cv_;
};

}  // namespace nexus::streamer::state
