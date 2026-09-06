#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/EventBus.h"
#include "core/IService.h"
#include "microphone/IMicrophoneHal.h"
#include "microphone/NoiseMonitor.h"
#include "microphone/SplMeter.h"

namespace nexus::microphone {

// Manages the internal measurement microphone. It is used ONLY for measurement, calibration, and
// monitoring — it never streams captured audio to the user or the cloud. Provides current SPL, an
// ambient-noise estimate, and a self-test that verifies the mic captures a non-silent signal.
// Capture is behind IMicrophoneHal so the logic is testable off-target.
class MicrophoneManager : public core::IService {
 public:
  MicrophoneManager(core::EventBus* bus, std::unique_ptr<IMicrophoneHal> hal = nullptr);

  std::string name() const override { return "microphone"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Capture `frames` samples and return the measured SPL (approximate dB).
  core::Result<double> measureSpl(std::size_t frames = 4800);

  // Capture and update the ambient-noise estimate; returns the current ambient level (dBFS).
  core::Result<double> updateAmbient(std::size_t frames = 4800);
  double ambientDbfs() const { return noise_.ambientDbfs(); }

  // Verify the mic is functional: capture must yield a buffer with some signal energy. Emits
  // MicFailure on failure.
  core::Status selfTest();

  // Direct capture access for calibration (Phase 7). Raw PCM; never leaves the device.
  core::Result<std::vector<std::int16_t>> capture(std::size_t frames);

 private:
  core::EventBus* bus_;
  std::unique_ptr<IMicrophoneHal> hal_;
  SplMeter spl_;
  NoiseMonitor noise_;
  bool open_ = false;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::microphone
