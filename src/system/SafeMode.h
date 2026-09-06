#pragma once

#include <atomic>
#include <functional>

#include "core/EventBus.h"

namespace nexus::system {

// Safe Mode is the last-resort degraded state the device enters when it can't run normally:
// repeated boot failures, a failed update, corrupt configuration, or the audio/DSP subsystem
// failing to start. In Safe Mode the amplifier stays muted and normal playback is disabled, but the
// technician web interface, logs, and network stay available so the device can be recovered
// (rollback / reset) remotely.
//
// The actual "mute amp + stop playback" action is injected so this stays decoupled; SystemManager
// owns a SafeMode instance and drives the state transition.
class SafeMode {
 public:
  // Action to disable normal operation (mute amp, stop audio). Called on enter().
  using DisableFn = std::function<void()>;

  explicit SafeMode(core::EventBus* bus) : bus_(bus) {}

  void setDisableAction(DisableFn fn) { disable_ = std::move(fn); }

  bool active() const { return active_.load(); }

  // Enter safe mode (idempotent). Runs the disable action and emits SafeModeEntered.
  void enter(const std::string& reason) {
    if (active_.exchange(true)) return;
    if (disable_) disable_();
    if (bus_) bus_->publish(core::Event{core::EventType::SafeModeEntered, "system",
                                        {{"reason", reason}}});
  }

 private:
  core::EventBus* bus_;
  DisableFn disable_;
  std::atomic<bool> active_{false};
};

}  // namespace nexus::system
