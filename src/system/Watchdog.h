#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::system {

// Monitors registered services on a periodic loop, calling healthCheck() on each. A service that
// reports unhealthy for `fail_threshold` consecutive polls triggers a recovery action via the
// registered callback (the escalation policy — restart module → restart app → reboot → safe mode —
// lives in SystemManager). Emits HealthCheckFailed per failing service and WatchdogTimeout when a
// recovery is requested. On the Pi it also pings the systemd watchdog (sd_notify WATCHDOG=1) each
// tick; off-target that is a no-op.
class Watchdog : public core::IService {
 public:
  // RecoveryFn: invoked when a watched service crosses the failure threshold; receives the service
  // name so the policy can act.
  using RecoveryFn = std::function<void(const std::string& service)>;

  explicit Watchdog(core::EventBus* bus = nullptr, int poll_ms = 5000, int fail_threshold = 3);

  std::string name() const override { return "watchdog"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  void watch(core::IService* svc);
  void setRecoveryHandler(RecoveryFn fn) { recovery_ = std::move(fn); }

  // Poll every watched service once; returns the number found unhealthy. Also updates the
  // consecutive-failure counters and fires recovery when a service crosses the threshold.
  int pollOnce();

 private:
  void loop();

  core::EventBus* bus_;
  int poll_ms_;
  int fail_threshold_;
  RecoveryFn recovery_;

  std::mutex mutex_;
  std::vector<core::IService*> watched_;
  std::unordered_map<std::string, int> fail_counts_;

  std::atomic<bool> running_{false};
  std::condition_variable cv_;
  std::thread thread_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::system
