#include "system/Watchdog.h"

#include <chrono>

#include "logging/Logger.h"

namespace nexus::system {

using core::ServiceState;
using core::Status;

Watchdog::Watchdog(core::EventBus* bus, int poll_ms, int fail_threshold)
    : bus_(bus), poll_ms_(poll_ms), fail_threshold_(fail_threshold) {}

void Watchdog::watch(core::IService* svc) {
  std::lock_guard<std::mutex> lock(mutex_);
  watched_.push_back(svc);
}

Status Watchdog::start() {
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  state_ = ServiceState::Running;
  return Status::success();
}

Status Watchdog::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  state_ = ServiceState::Stopped;
  return Status::success();
}

int Watchdog::pollOnce() {
  std::vector<core::IService*> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = watched_;
  }

  int unhealthy = 0;
  for (auto* svc : snapshot) {
    if (!svc || svc == this) continue;
    const bool ok = svc->healthCheck().ok();
    const std::string n = svc->name();
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
      fail_counts_[n] = 0;
      continue;
    }
    ++unhealthy;
    int count = ++fail_counts_[n];
    NX_LOG_WARN("watchdog", "service '" + n + "' unhealthy (" + std::to_string(count) + "/" +
                                std::to_string(fail_threshold_) + ")");
    if (bus_) bus_->publish(core::Event{core::EventType::HealthCheckFailed, "watchdog",
                                        {{"service", n}, {"count", count}}});
    if (count >= fail_threshold_) {
      fail_counts_[n] = 0;  // reset so we don't fire every tick
      if (bus_) bus_->publish(core::Event{core::EventType::WatchdogTimeout, "watchdog",
                                          {{"service", n}}});
      if (recovery_) recovery_(n);
    }
  }
  return unhealthy;
}

void Watchdog::loop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    cv_.wait_for(lock, std::chrono::milliseconds(poll_ms_), [this] { return !running_; });
    if (!running_) break;
    lock.unlock();
    pollOnce();
    // TODO(Pi): sd_notify("WATCHDOG=1") here to feed the systemd hardware watchdog.
    lock.lock();
  }
}

}  // namespace nexus::system
