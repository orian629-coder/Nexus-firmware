#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace nexus::measure {

// Test stimuli for acoustic distance measurement.
//
// Two shapes, because they answer different questions:
//
//   • Chirp — a logarithmic sweep. Spreads energy over time, so it can be played quietly and still
//     correlate strongly. That matters in a real room, where an impulse loud enough to beat the
//     noise floor is unpleasantly loud to stand next to.
//   • Impulse — a single sample. Gives the sharpest possible correlation peak, which is what you
//     want for measuring HARDWARE loopback latency, where there is no room noise to fight.
//
// The AOA prototype used exactly this split (chirp for distance, impulse for RTL), and those
// parameters are carried over here rather than re-derived.

// Logarithmic sweep from f0 to f1 over `duration` seconds, windowed.
//
// The Hanning window is not cosmetic: an abruptly-started sweep has a step discontinuity whose
// broadband click correlates almost as well as the sweep itself, producing a second peak that can
// win and put the measured arrival a whole window early.
inline std::vector<double> logChirp(double duration_s, double sample_rate, double f0 = 1000.0,
                                    double f1 = 15000.0, double amplitude = 0.4) {
  const std::size_t n = static_cast<std::size_t>(duration_s * sample_rate);
  std::vector<double> out(n);
  if (n == 0) return out;

  // Log sweep: instantaneous frequency rises geometrically, so phase is the integral of that.
  const double k = std::log(f1 / f0);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const double phase = 2.0 * M_PI * f0 * duration_s / k * (std::exp(t * k / duration_s) - 1.0);
    // Hanning window over the whole sweep.
    const double w =
        0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n - 1)));
    out[i] = amplitude * std::sin(phase) * w;
  }
  return out;
}

// A single non-zero sample at the midpoint of an otherwise silent buffer.
//
// Centred rather than at index 0 so the correlation peak has room on both sides: a peak at the very
// edge cannot be distinguished from one that fell outside the captured window.
inline std::vector<double> impulse(double duration_s, double sample_rate,
                                   double amplitude = 0.4) {
  const std::size_t n = static_cast<std::size_t>(duration_s * sample_rate);
  std::vector<double> out(n, 0.0);
  if (n > 0) out[n / 2] = amplitude;
  return out;
}

}  // namespace nexus::measure
