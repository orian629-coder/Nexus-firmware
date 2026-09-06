#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/EventBus.h"
#include "core/Result.h"
#include "system/SystemState.h"

namespace nexus::system {

// Enforces legal transitions among the 14 system states via a static table. Illegal transitions
// are rejected (returned as an error and logged) but never throw or crash — a caller requesting
// an invalid move is a bug to surface, not a reason to take down the process. Every accepted
// transition emits a StateChanged event on the bus so other modules can react.
class StateMachine {
 public:
  explicit StateMachine(core::EventBus* bus = nullptr);

  SystemState current() const;

  // Validate and apply a transition. `reason` is recorded in the emitted event and the log.
  core::Status transitionTo(SystemState to, std::string reason, std::string command_id = {});

  bool isLegal(SystemState from, SystemState to) const;

  // Register a callback fired (synchronously, after the event is published) when a given state
  // is entered — used for recovery hooks (e.g. Degraded/Error).
  using EnterHook = std::function<void(SystemState from, SystemState to)>;
  void onEnter(SystemState state, EnterHook hook);

 private:
  core::EventBus* bus_;
  mutable std::mutex mutex_;
  SystemState current_ = SystemState::Booting;
  std::vector<std::pair<SystemState, EnterHook>> hooks_;
};

}  // namespace nexus::system
