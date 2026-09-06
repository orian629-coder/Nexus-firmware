#include "system/SystemManager.h"

#include "logging/Logger.h"

namespace nexus::system {

using core::Status;

SystemManager::SystemManager(core::EventBus* bus)
    : bus_(bus), machine_(bus), safe_mode_(bus) {}

void SystemManager::handleEvent(const core::Event& ev) {
  // Ignore connectivity events until the initial state is resolved — during BOOTING the network
  // may already be up (e.g. Ethernet), but the config/pairing gate (enterInitialState) decides
  // where we go first.
  if (machine_.current() == SystemState::Booting &&
      (ev.type == core::EventType::NetworkConnected ||
       ev.type == core::EventType::NetworkDisconnected)) {
    return;
  }

  switch (ev.type) {
    case core::EventType::NetworkConnected:
      // From SETUP_MODE the network coming up IS the setup succeeding: the user just handed over
      // their Wi-Fi credentials and the device joined. SETUP_MODE cannot reach SearchingStreamer
      // directly (see legalTransition), so route through ConnectingNetwork — which then advances
      // to SearchingStreamer. Jumping straight there left the device wedged in SETUP_MODE,
      // re-logging "illegal transition" on every connectivity poll.
      if (machine_.current() == SystemState::SetupMode) {
        machine_.transitionTo(SystemState::ConnectingNetwork, "network connected (setup done)");
      }
      // NetworkConnected is republished by the connectivity monitor every few seconds (on purpose,
      // so consumers can re-assert derived state such as taking the setup hotspot down). It must
      // therefore be idempotent HERE too: once the device has moved past discovery, "the network is
      // still up" is not news and must not drag it backwards. Without this guard the poll knocked a
      // playing speaker out of PLAYING every 5 s — observed on real hardware as
      // "illegal transition PLAYING -> SEARCHING_STREAMER (network connected)" — and, worse, reset
      // AUTHENTICATING on every tick so a handshake slower than the poll could never finish.
      switch (machine_.current()) {
        case SystemState::Authenticating:
        case SystemState::Online:
        case SystemState::Playing:
        case SystemState::Calibrating:
        case SystemState::Updating:
          break;  // already past discovery — nothing to do
        default:
          machine_.transitionTo(SystemState::SearchingStreamer, "network connected");
          break;
      }
      break;
    case core::EventType::NetworkDisconnected:
      machine_.transitionTo(SystemState::Offline, "network lost");
      break;
    case core::EventType::StreamerFound:
      machine_.transitionTo(SystemState::Authenticating, "streamer found");
      break;
    case core::EventType::PairingCompleted:
      // PairingCompleted means two different things depending on where we are: finishing initial
      // setup (SetupMode → join the network next) vs. authenticating to the streamer while online
      // (Authenticating → Online). Route accordingly.
      if (machine_.current() == SystemState::SetupMode) {
        machine_.transitionTo(SystemState::ConnectingNetwork, "paired, joining network");
      } else {
        machine_.transitionTo(SystemState::Online, "authenticated");
      }
      error_streak_ = 0;
      break;
    case core::EventType::StreamerDisconnected:
      machine_.transitionTo(SystemState::Offline, "streamer disconnected");
      break;
    case core::EventType::AudioStarted:
      // Audio arriving IS evidence that a streamer is present and talking to us, even if the
      // discovery handshake never completed — the UDP receiver is always listening and needs no
      // handshake, so a speaker can legitimately be streaming while still SEARCHING_STREAMER (it
      // happens whenever the streamer does not advertise over mDNS, or the beacon was missed).
      // Without this the transition to Playing is rejected and /api/status reports
      // SEARCHING_STREAMER while sound is actually coming out of the speaker — observed on real
      // hardware. Route through Online so the state machine reaches Playing along a legal edge.
      if (machine_.current() == SystemState::SearchingStreamer ||
          machine_.current() == SystemState::Authenticating) {
        machine_.transitionTo(SystemState::Online, "audio arriving from a streamer");
      }
      machine_.transitionTo(SystemState::Playing, "audio started");
      break;
    case core::EventType::AudioStopped:
      machine_.transitionTo(SystemState::Online, "audio stopped");
      break;
    case core::EventType::ConfigInvalid:
      machine_.transitionTo(SystemState::Degraded, "config invalid");
      // Corrupt config is a recovery-worthy fault; escalate to Safe Mode so the device can be
      // recovered remotely rather than looping.
      enterSafeMode("config invalid");
      break;
    case core::EventType::IdentityCorrupt:
      machine_.transitionTo(SystemState::Error, "identity corrupt");
      enterSafeMode("identity corrupt");
      break;
    case core::EventType::UpdateFailed:
      // A failed update that couldn't roll back leaves us in a shaky state.
      if (++error_streak_ >= 3) enterSafeMode("repeated update failures");
      break;
    case core::EventType::WatchdogTimeout:
      machine_.transitionTo(SystemState::Degraded, "watchdog timeout");
      if (++error_streak_ >= 3) enterSafeMode("repeated watchdog timeouts");
      break;
    case core::EventType::ShutdownRequested:
      machine_.transitionTo(SystemState::ShuttingDown, "shutdown requested");
      break;
    default:
      break;
  }
}

Status SystemManager::start() {
  // Guards inside transitionTo reject illegal moves, so it's safe to attempt from any state.
  sub_ = bus_->subscribeAll([this](const core::Event& ev) { handleEvent(ev); });
  state_ = core::ServiceState::Running;
  return Status::success();
}

Status SystemManager::stop() {
  if (sub_) {
    bus_->unsubscribe(sub_);
    sub_ = 0;
  }
  machine_.transitionTo(SystemState::ShuttingDown, "service stop");
  state_ = core::ServiceState::Stopped;
  return Status::success();
}

void SystemManager::enterInitialState(bool configured) {
  if (configured) {
    machine_.transitionTo(SystemState::ConnectingNetwork, "configured at boot");
  } else {
    machine_.transitionTo(SystemState::Unconfigured, "no configuration at boot");
  }
}

void SystemManager::enterSafeMode(const std::string& reason) {
  if (safe_mode_.active()) return;
  NX_LOG_CRIT("system", core::ErrorCode::Unknown, "entering SAFE MODE: " + reason);
  safe_mode_.enter(reason);
  machine_.transitionTo(SystemState::Degraded, "safe mode: " + reason);
}

}  // namespace nexus::system
