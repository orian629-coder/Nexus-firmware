#include "status/StatusService.h"

#include "logging/Logger.h"

namespace nexus::status {

using core::Event;
using core::EventType;
using core::ServiceState;
using core::Status;

StatusService::StatusService(core::EventBus* bus, identity::DeviceIdentity* identity,
                             config::ConfigManager* config, Clock clock, Sink sink,
                             int heartbeat_seconds)
    : bus_(bus),
      identity_(identity),
      config_(config),
      clock_(std::move(clock)),
      sink_(std::move(sink)),
      heartbeat_seconds_(heartbeat_seconds) {}

nlohmann::json StatusService::buildHeartbeat() const {
  const auto& cfg = config_->get();
  return {
      {"type", "heartbeat"},
      {"device_id", identity_ ? identity_->deviceId() : ""},
      {"state", "ONLINE"},
      {"volume", cfg.audio.volume},
      {"muted", cfg.audio.muted},
      {"software_version", cfg.device.software_version},
      {"paired", cfg.pairing.paired},
      {"timestamp", clock_()},
  };
}

void StatusService::dispatch(const nlohmann::json& payload) {
  if (sink_) {
    sink_(payload);
  } else if (bus_) {
    // Default sink: surface on the bus so other modules (and, later, the streamer-push transport)
    // can observe it. Never contains secrets.
    bus_->publish(Event{EventType::CommandExecuted, "status", payload});
  }
}

void StatusService::sendHeartbeat() {
  dispatch(buildHeartbeat());
  heartbeat_count_.fetch_add(1);
}

void StatusService::onEvent(const Event& ev) {
  // Critical events are reported immediately rather than waiting for the next heartbeat.
  const char* alert = nullptr;
  switch (ev.type) {
    case EventType::AmplifierOverheat: alert = "AMPLIFIER_OVERHEAT"; break;
    case EventType::AmplifierFault: alert = "AMPLIFIER_FAULT"; break;
    case EventType::AmplifierProtection: alert = "AMPLIFIER_PROTECTION"; break;
    case EventType::AudioLost: alert = "AUDIO_LOST"; break;
    case EventType::StreamerDisconnected: alert = "STREAMER_DISCONNECTED"; break;
    case EventType::UpdateFailed: alert = "UPDATE_FAILED"; break;
    case EventType::MicFailure: alert = "MICROPHONE_FAILURE"; break;
    case EventType::TempWarning: alert = "TEMP_WARNING"; break;
    default: return;
  }
  nlohmann::json a = {
      {"type", "alert"},
      {"device_id", identity_ ? identity_->deviceId() : ""},
      {"alert", alert},
      {"timestamp", clock_()},
  };
  NX_LOG_WARN("status", std::string("immediate alert: ") + alert);
  dispatch(a);
}

Status StatusService::start() {
  sub_ = bus_->subscribeAll([this](const Event& ev) { onEvent(ev); });
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  state_ = ServiceState::Running;
  return Status::success();
}

void StatusService::loop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    // Wait heartbeat_seconds_ or until stopped. Using a CV so stop() is prompt.
    cv_.wait_for(lock, std::chrono::seconds(heartbeat_seconds_), [this] { return !running_; });
    if (!running_) break;
    lock.unlock();
    sendHeartbeat();
    lock.lock();
  }
}

Status StatusService::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (sub_) {
    bus_->unsubscribe(sub_);
    sub_ = 0;
  }
  state_ = ServiceState::Stopped;
  return Status::success();
}

}  // namespace nexus::status
