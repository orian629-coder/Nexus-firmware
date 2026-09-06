#pragma once

#include <array>
#include <string>

#include "core/Result.h"
#include "dsp/Equalizer.h"

namespace nexus::calibration {

// A saved calibration result: the correction EQ gains plus metadata.
struct CalibrationProfile {
  std::string name = "default";
  std::array<double, dsp::kEqBands> eq_gains{};
  double score = 0.0;
  double room_noise_dbfs = 0.0;
};

// Persists calibration profiles as JSON under a directory (default
// /var/lib/nexus-speaker/calibration), atomically. Profiles survive reboot and are re-applied to
// the DSP on startup.
class CalibrationProfiles {
 public:
  explicit CalibrationProfiles(std::string dir = "/var/lib/nexus-speaker/calibration");

  core::Status save(const CalibrationProfile& profile);
  core::Result<CalibrationProfile> load(const std::string& name) const;
  bool has(const std::string& name) const;

 private:
  std::string pathFor(const std::string& name) const;
  std::string dir_;
};

}  // namespace nexus::calibration
