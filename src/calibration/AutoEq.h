#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "dsp/Equalizer.h"

namespace nexus::calibration {

// Computes an EQ correction curve from a room measurement. Given the measured per-band energy (dB)
// of a known-flat calibration signal (pink noise), the correction for each band is the negative
// deviation from the average level — bands that came back quiet get boosted, loud bands get cut —
// toward a flat in-room response. Gains are clamped to the EQ's ±10 dB range. Pure and testable.
class AutoEq {
 public:
  // `smoothing` in [0,1] scales the correction strength (1 = full inverse, lower = gentler).
  explicit AutoEq(double smoothing = 1.0) : smoothing_(smoothing) {}

  std::array<double, dsp::kEqBands> computeCorrection(
      const std::array<double, dsp::kEqBands>& measured_db) const {
    // Target = the average measured level (flat relative to the overall loudness).
    double avg = 0.0;
    for (double v : measured_db) avg += v;
    avg /= static_cast<double>(dsp::kEqBands);

    std::array<double, dsp::kEqBands> gains{};
    for (int b = 0; b < dsp::kEqBands; ++b) {
      double correction = (avg - measured_db[b]) * smoothing_;  // boost quiet, cut loud
      gains[b] = std::clamp(correction, -10.0, 10.0);
    }
    return gains;
  }

  // A simple quality score [0,100]: 100 = perfectly flat measurement, lower = more deviation.
  static double flatnessScore(const std::array<double, dsp::kEqBands>& measured_db) {
    double avg = 0.0;
    for (double v : measured_db) avg += v;
    avg /= static_cast<double>(dsp::kEqBands);
    double var = 0.0;
    for (double v : measured_db) var += (v - avg) * (v - avg);
    var /= static_cast<double>(dsp::kEqBands);
    const double stddev = std::sqrt(var);
    // Map stddev (dB) to a score: 0 dB → 100, 20 dB → 0.
    return std::clamp(100.0 - stddev * 5.0, 0.0, 100.0);
  }

 private:
  double smoothing_;
};

}  // namespace nexus::calibration
