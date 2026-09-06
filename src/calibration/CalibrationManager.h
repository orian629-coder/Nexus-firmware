#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "calibration/AutoEq.h"
#include "calibration/CalibrationProfiles.h"
#include "calibration/RoomMeasurement.h"
#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::calibration {

// Calibration state machine from the spec.
enum class CalState {
  Idle,
  Preparing,
  Measuring,
  Analyzing,
  Applying,
  Verifying,
  Completed,
  Failed,
};

const char* toString(CalState s);

struct CalibrationResult {
  std::array<double, dsp::kEqBands> recommended_eq{};
  double room_noise_dbfs = 0.0;
  double score = 0.0;
};

// Orchestrates automatic acoustic calibration:
//   mute playback → play calibration signal → capture mic → analyze (RoomMeasurement + AutoEq) →
//   apply EQ → verify → save profile → restore playback.
//
// The hardware-touching steps (play signal, capture, apply EQ to the DSP) are injected as hooks so
// the orchestration is fully testable off-target and the module stays decoupled from audio/dsp/mic.
class CalibrationManager : public core::IService {
 public:
  // PlaySignal: emit the calibration tone and return the captured mic PCM for analysis.
  //   (frames requested) -> captured int16 PCM
  using CaptureFn = std::function<std::vector<std::int16_t>(std::size_t frames)>;
  // ApplyEqFn: push the correction gains into the DSP.
  using ApplyEqFn = std::function<void(const std::array<double, dsp::kEqBands>&)>;
  // MuteFn: mute/unmute normal playback around the measurement.
  using MuteFn = std::function<void(bool muted)>;

  CalibrationManager(core::EventBus* bus, config::ConfigManager* config,
                     std::shared_ptr<CalibrationProfiles> profiles, double sample_rate = 48000.0);

  std::string name() const override { return "calibration"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return svc_state_; }

  void setHooks(CaptureFn capture, ApplyEqFn apply_eq, MuteFn mute);

  CalState calState() const;

  // Run a full calibration synchronously. Emits CalibrationStarted/Completed/Failed and, on
  // success, applies + persists the profile and updates config's eq_profile.
  core::Result<CalibrationResult> runCalibration(const std::string& profile_name = "room");

  // Re-apply a saved profile (called on boot if a calibration exists).
  core::Status applySavedProfile(const std::string& profile_name);

 private:
  void setState(CalState s);

  core::EventBus* bus_;
  config::ConfigManager* config_;
  std::shared_ptr<CalibrationProfiles> profiles_;
  RoomMeasurement room_;
  AutoEq auto_eq_;

  CaptureFn capture_;
  ApplyEqFn apply_eq_;
  MuteFn mute_;

  mutable std::mutex mutex_;
  CalState cal_state_ = CalState::Idle;
  core::ServiceState svc_state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::calibration
