#pragma once

#include <memory>

#include "core/EventBus.h"
#include "core/IService.h"
#include "system/SafeMode.h"
#include "system/StateMachine.h"

namespace nexus::system {

// Owns the StateMachine and translates inbound module events into state transitions. It maps
// network/pairing/audio events to the normal flow and drives recovery: config corruption, a failed
// update, or repeated errors escalate to Safe Mode (amp muted, playback off, web/logs/network up)
// so the device can be recovered remotely.
class SystemManager : public core::IService {
 public:
  explicit SystemManager(core::EventBus* bus);

  std::string name() const override { return "system"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  StateMachine& states() { return machine_; }
  SafeMode& safeMode() { return safe_mode_; }

  // Wire the action Safe Mode runs to disable normal operation (mute amp, stop audio).
  void setSafeModeDisableAction(SafeMode::DisableFn fn) { safe_mode_.setDisableAction(std::move(fn)); }

  // Called by Application once configuration/pairing status is known, to move out of Booting.
  void enterInitialState(bool configured);

  // Force Safe Mode (used by Application on repeated boot failure).
  void enterSafeMode(const std::string& reason);

 private:
  void handleEvent(const core::Event& ev);

  core::EventBus* bus_;
  StateMachine machine_;
  SafeMode safe_mode_;
  int error_streak_ = 0;  // consecutive error-ish events → escalate
  core::EventBus::Token sub_ = 0;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::system
