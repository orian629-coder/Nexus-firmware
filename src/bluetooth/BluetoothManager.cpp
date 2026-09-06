#include "bluetooth/BluetoothManager.h"

#include "logging/Logger.h"

#if NEXUS_STUB_HAL
// StubBluetoothHal is defined in the interface header.
#else
#include "bluetooth/BluetoothctlHal.h"
#endif

namespace nexus::bluetooth {

using core::ErrorCode;
using core::Event;
using core::EventType;
using core::Result;
using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IBluetoothHal> makeDefaultHal() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubBluetoothHal>();
#else
  return std::make_unique<BluetoothctlHal>();
#endif
}
}  // namespace

BluetoothManager::BluetoothManager(core::EventBus* bus, std::string alias,
                                   std::unique_ptr<IBluetoothHal> hal)
    : bus_(bus), alias_(std::move(alias)), hal_(hal ? std::move(hal) : makeDefaultHal()) {}

Status BluetoothManager::start() {
  auto st = hal_->status();
  if (!st.ok() || !st.value().available) {
    // No adapter: optional service, degrade rather than fault.
    NX_LOG_WARN("bluetooth", "no Bluetooth adapter available; running degraded");
    state_ = ServiceState::Degraded;
    return Status::success();
  }

  Status s = hal_->enableSink(alias_);
  if (!s.ok()) {
    NX_LOG_ERROR("bluetooth", s.code(), "failed to enable A2DP sink: " + s.message());
    state_ = ServiceState::Degraded;
    return Status::success();  // non-critical: never take down the process
  }

  NX_LOG_INFO("bluetooth", "A2DP sink ready, advertising as \"" + alias_ + "\"");
  state_ = ServiceState::Running;
  refreshConnection();  // reflect any device already connected at boot
  return Status::success();
}

Status BluetoothManager::stop() {
  if (hal_) hal_->disable();
  state_ = ServiceState::Stopped;
  connected_ = false;
  device_name_.clear();
  return Status::success();
}

Status BluetoothManager::healthCheck() {
  // Degraded (no adapter) is a healthy terminal state for an optional service.
  if (state_ == ServiceState::Degraded) return Status::success();
  refreshConnection();
  return state_ == ServiceState::Running
             ? Status::success()
             : Status::error(ErrorCode::Unknown, "bluetooth not running");
}

Status BluetoothManager::openPairingWindow(int seconds) {
  if (state_ != ServiceState::Running) {
    return Status::error(ErrorCode::Unknown, "bluetooth not running");
  }
  NX_LOG_INFO("bluetooth", "opening pairing window (" +
                               (seconds > 0 ? std::to_string(seconds) + "s" : "no timeout") + ")");
  return hal_->openPairingWindow(seconds);
}

void BluetoothManager::refreshConnection() {
  auto st = hal_->status();
  if (!st.ok()) return;
  const bool now = st.value().connected;
  if (now == connected_) return;

  connected_ = now;
  device_name_ = now ? st.value().device_name : std::string{};
  if (now) {
    NX_LOG_INFO("bluetooth", "device connected: " + device_name_);
    bus_->publish(Event(EventType::AudioStarted, name(), {{"source", "bluetooth"},
                                                          {"device", device_name_}}));
  } else {
    NX_LOG_INFO("bluetooth", "device disconnected");
    bus_->publish(Event(EventType::AudioStopped, name(), {{"source", "bluetooth"}}));
  }
}

}  // namespace nexus::bluetooth
