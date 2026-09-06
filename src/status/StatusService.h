#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "identity/DeviceIdentity.h"

namespace nexus::status {

// Builds and dispatches the speaker's status to the Streamer:
//  - a periodic heartbeat (default every 30 s), and
//  - immediate alerts for critical events (overheat, amp fault, audio lost, streamer
//    disconnected, update failed, mic failure) that must not wait for the next heartbeat.
//
// The actual delivery is pluggable via a Sink (default: publish on the bus + log), so the module
// is fully testable and the real streamer-push transport can be injected later. The heartbeat
// builder pulls current state from config + identity + a supplied temperature/telemetry provider.
class StatusService : public core::IService {
 public:
  using Sink = std::function<void(const nlohmann::json&)>;
  using Clock = std::function<std::int64_t()>;  // epoch seconds

  StatusService(core::EventBus* bus, identity::DeviceIdentity* identity,
                config::ConfigManager* config, Clock clock, Sink sink = nullptr,
                int heartbeat_seconds = 30);

  std::string name() const override { return "status"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Build the current heartbeat payload (also used by GET_STATUS-style consumers and tests).
  nlohmann::json buildHeartbeat() const;

  // Send one heartbeat now (used by the periodic loop and tests).
  void sendHeartbeat();

  // Number of heartbeats dispatched (test aid).
  std::uint64_t heartbeatCount() const { return heartbeat_count_.load(); }

 private:
  void loop();
  void onEvent(const core::Event& ev);
  void dispatch(const nlohmann::json& payload);

  core::EventBus* bus_;
  identity::DeviceIdentity* identity_;
  config::ConfigManager* config_;
  Clock clock_;
  Sink sink_;
  int heartbeat_seconds_;

  core::EventBus::Token sub_ = 0;
  std::atomic<std::uint64_t> heartbeat_count_{0};
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::status
