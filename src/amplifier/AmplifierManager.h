#pragma once

#include <memory>
#include <mutex>

#include "amplifier/IAmplifierHal.h"
#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::amplifier {

// Amplifier operating states from the spec.
enum class AmpState {
  Off,
  Starting,
  Ready,
  Muted,
  Protection,
  Overheated,
  Fault,
};

const char* toString(AmpState s);

// Controls and monitors the power amplifier. On boot it powers on muted, then unmutes once the
// hardware is stable. It polls temperature and fault/protection flags; crossing the warning
// threshold emits TempWarning, and overheat/fault/protection transition the state machine and
// emit the matching immediate alerts (which StatusService forwards to the Streamer). All hardware
// access is behind IAmplifierHal so the logic is testable off-target.
class AmplifierManager : public core::IService {
 public:
  AmplifierManager(core::EventBus* bus, std::unique_ptr<IAmplifierHal> hal = nullptr,
                   double warn_celsius = 70.0, double shutdown_celsius = 85.0);

  std::string name() const override { return "amplifier"; }
  core::Status start() override;  // power on (muted) → Starting
  core::Status stop() override;   // mute + power off → Off
  core::ServiceState state() const override { return svc_state_; }
  core::Status healthCheck() override;

  AmpState ampState() const;

  // Unmute after the amp is confirmed stable (called once audio is ready). Ignored unless Ready/
  // Muted and no active fault.
  core::Status unmute();
  core::Status mute();

  // Poll temperature + flags once and update the state machine. Called periodically by the
  // service loop (or directly by tests). Returns the current temperature.
  core::Result<double> poll();

 private:
  void transition(AmpState to, const char* reason);

  core::EventBus* bus_;
  std::unique_ptr<IAmplifierHal> hal_;
  double warn_celsius_;
  double shutdown_celsius_;

  mutable std::mutex mutex_;
  AmpState amp_state_ = AmpState::Off;
  bool warned_ = false;      // avoid repeated TempWarning spam
  bool no_hardware_ = false; // no controllable amp (e.g. DAC-only HAT) — mute/unmute are no-ops
  core::ServiceState svc_state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::amplifier
