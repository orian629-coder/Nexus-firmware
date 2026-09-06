#pragma once

#include <cstdint>
#include <vector>

#include "microphone/SplMeter.h"

namespace nexus::microphone {

// Tracks ambient background noise via an exponentially-weighted moving average of the SPL floor.
// Feeds Auto Volume (Phase 7), which raises playback a fixed offset above the ambient noise.
class NoiseMonitor {
 public:
  explicit NoiseMonitor(double smoothing = 0.2) : smoothing_(smoothing) {}

  // Update with one measurement block; returns the current ambient estimate (dB).
  double update(const std::vector<std::int16_t>& pcm) {
    const double level = SplMeter::rmsDbfs(pcm);
    if (!have_) {
      ambient_ = level;
      have_ = true;
    } else {
      ambient_ += smoothing_ * (level - ambient_);
    }
    return ambient_;
  }

  double ambientDbfs() const { return ambient_; }
  void reset() {
    have_ = false;
    ambient_ = -120.0;
  }

 private:
  double smoothing_;
  double ambient_ = -120.0;
  bool have_ = false;
};

}  // namespace nexus::microphone
