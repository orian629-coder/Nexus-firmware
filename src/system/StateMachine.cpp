#include "system/StateMachine.h"

#include <array>

#include "logging/Logger.h"

namespace nexus::system {

using core::ErrorCode;
using core::Status;

const char* toString(SystemState s) {
  switch (s) {
    case SystemState::Booting: return "BOOTING";
    case SystemState::Unconfigured: return "UNCONFIGURED";
    case SystemState::SetupMode: return "SETUP_MODE";
    case SystemState::ConnectingNetwork: return "CONNECTING_NETWORK";
    case SystemState::SearchingStreamer: return "SEARCHING_STREAMER";
    case SystemState::Authenticating: return "AUTHENTICATING";
    case SystemState::Online: return "ONLINE";
    case SystemState::Playing: return "PLAYING";
    case SystemState::Calibrating: return "CALIBRATING";
    case SystemState::Updating: return "UPDATING";
    case SystemState::Degraded: return "DEGRADED";
    case SystemState::Offline: return "OFFLINE";
    case SystemState::Error: return "ERROR";
    case SystemState::ShuttingDown: return "SHUTTING_DOWN";
  }
  return "UNKNOWN";
}

namespace {

bool contains(std::initializer_list<SystemState> set, SystemState s) {
  for (auto x : set) {
    if (x == s) return true;
  }
  return false;
}

// Legal targets per source state. Any state may go to ShuttingDown and to Error (fault path),
// handled below so the table stays focused on the normal flow.
bool legalTransition(SystemState from, SystemState to) {
  if (to == SystemState::ShuttingDown) return true;       // shutdown always allowed
  if (to == SystemState::Error && from != SystemState::ShuttingDown) return true;  // fault path
  if (from == to) return true;                            // idempotent self-transition

  switch (from) {
    case SystemState::Booting:
      return contains({SystemState::Unconfigured, SystemState::ConnectingNetwork}, to);
    case SystemState::Unconfigured:
      return contains({SystemState::SetupMode}, to);
    case SystemState::SetupMode:
      return contains({SystemState::ConnectingNetwork, SystemState::Unconfigured}, to);
    case SystemState::ConnectingNetwork:
      return contains({SystemState::SearchingStreamer, SystemState::Offline,
                       SystemState::SetupMode},
                      to);
    case SystemState::SearchingStreamer:
      // Online is reachable without Authenticating: the audio receiver needs no handshake, so a
      // streamer can start sending before (or without) being discovered over mDNS. Audio arriving
      // is itself proof that a streamer is present, and SystemManager routes through Online so the
      // device can reach Playing along a legal edge instead of being stuck reporting
      // SEARCHING_STREAMER while sound is coming out.
      return contains({SystemState::Authenticating, SystemState::Online, SystemState::Offline,
                       SystemState::ConnectingNetwork},
                      to);
    case SystemState::Authenticating:
      return contains({SystemState::Online, SystemState::SearchingStreamer, SystemState::Offline},
                      to);
    case SystemState::Online:
      return contains({SystemState::Playing, SystemState::Calibrating, SystemState::Updating,
                       SystemState::Degraded, SystemState::Offline},
                      to);
    case SystemState::Playing:
      return contains({SystemState::Online, SystemState::Calibrating, SystemState::Degraded,
                       SystemState::Offline},
                      to);
    case SystemState::Calibrating:
      return contains({SystemState::Online, SystemState::Playing, SystemState::Degraded}, to);
    case SystemState::Updating:
      return contains({SystemState::Online}, to);
    case SystemState::Degraded:
      return contains({SystemState::Online, SystemState::Offline}, to);
    case SystemState::Offline:
      return contains({SystemState::ConnectingNetwork, SystemState::SearchingStreamer,
                       SystemState::Degraded},
                      to);
    case SystemState::Error:
      return contains({SystemState::Booting, SystemState::Degraded}, to);  // recovery
    case SystemState::ShuttingDown:
      return false;  // terminal
  }
  return false;
}

}  // namespace

StateMachine::StateMachine(core::EventBus* bus) : bus_(bus) {}

SystemState StateMachine::current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

bool StateMachine::isLegal(SystemState from, SystemState to) const {
  return legalTransition(from, to);
}

void StateMachine::onEnter(SystemState state, EnterHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  hooks_.emplace_back(state, std::move(hook));
}

Status StateMachine::transitionTo(SystemState to, std::string reason, std::string command_id) {
  SystemState from;
  std::vector<EnterHook> to_fire;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    from = current_;
    // A self-transition (from == to) is a no-op: nothing changed, so don't log, re-publish
    // StateChanged, or re-fire enter hooks. This matters now that connectivity is re-published every
    // monitor poll — without it, a stable "network connected" would spam an identical
    // SEARCHING_STREAMER -> SEARCHING_STREAMER line every few seconds.
    if (from == to) return Status::success();
    if (!legalTransition(from, to)) {
      NX_LOG_WARN("state", std::string("illegal transition ") + toString(from) + " -> " +
                               toString(to) + " (" + reason + ")");
      return Status::error(ErrorCode::IllegalStateTransition,
                           std::string(toString(from)) + " -> " + toString(to));
    }
    current_ = to;
    for (auto& [st, hook] : hooks_) {
      if (st == to) to_fire.push_back(hook);
    }
  }

  NX_LOG_INFO("state",
              std::string("transition ") + toString(from) + " -> " + toString(to) + " (" +
                  reason + ")");
  if (bus_) {
    core::Event ev{core::EventType::StateChanged, "system"};
    ev.command_id = command_id;
    ev.data = {{"from", toString(from)}, {"to", toString(to)}, {"reason", reason}};
    bus_->publish(std::move(ev));
  }
  for (auto& hook : to_fire) {
    if (hook) hook(from, to);
  }
  return Status::success();
}

}  // namespace nexus::system
