#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace nexus::measure {

// Where the correlation peaked, and how much to trust it.
struct Arrival {
  long lag_samples = 0;     // how far the recording lags the stimulus
  double lag_ms = 0.0;
  double peak = 0.0;        // normalized correlation at the peak, 0..1 for identical signals
  double second_peak = 0.0; // best peak outside the main lobe — the competing candidate
  bool found = false;

  // Ratio of the winning peak to the next-best unrelated one. A confident detection has one clear
  // winner; a room echo, a wrong device, or pure noise produces several similar peaks. Reported
  // rather than thresholded here so the caller decides what is good enough for its own use.
  double confidence() const {
    if (!found || second_peak <= 0.0) return found ? 1.0 : 0.0;
    return peak / second_peak;
  }
};

// Subtract the mean and scale to unit norm.
//
// Both steps matter. The mean removal kills any DC offset — a converter with even a small DC bias
// correlates its own offset against the stimulus's, which biases the peak. The norm scaling makes
// the peak value comparable between recordings at different levels, so `peak` means "how well did
// this match" rather than "how loud was it".
inline std::vector<double> normalize(const std::vector<double>& x) {
  std::vector<double> out = x;
  if (out.empty()) return out;

  double mean = 0.0;
  for (double v : out) mean += v;
  mean /= static_cast<double>(out.size());

  double sumsq = 0.0;
  for (double& v : out) {
    v -= mean;
    sumsq += v * v;
  }
  const double norm = std::sqrt(sumsq);
  // The epsilon keeps digital silence from dividing by zero; such a signal correlates to ~0 and is
  // reported as not found rather than as a spurious match.
  const double scale = 1.0 / (norm + 1e-12);
  for (double& v : out) v *= scale;
  return out;
}

// Find where `stimulus` appears inside `recording` by cross-correlation.
//
// This is the measurement that everything else rests on: the lag in samples is the total round trip
// (hardware + network + flight time), and only after the known fixed delays are subtracted does the
// remainder mean distance.
//
// Direct O(n*m) correlation. For a 1 s stimulus at 44.1 kHz that is ~2e9 multiply-adds, which is
// slow enough to matter, so the search is restricted to `max_lag_samples` when the caller knows the
// answer cannot exceed some bound (it always does — the speed of sound and the room size bound it).
inline Arrival findArrival(const std::vector<double>& recording,
                           const std::vector<double>& stimulus, double sample_rate,
                           long max_lag_samples = -1) {
  Arrival a;
  if (recording.empty() || stimulus.empty() || sample_rate <= 0.0) return a;
  if (recording.size() < stimulus.size()) return a;

  const std::vector<double> r = normalize(recording);
  const std::vector<double> s = normalize(stimulus);

  const long max_possible = static_cast<long>(r.size() - s.size());
  const long limit = (max_lag_samples < 0 || max_lag_samples > max_possible) ? max_possible
                                                                            : max_lag_samples;

  // Only non-negative lags are searched: the recording cannot contain the stimulus before it was
  // played, so a negative lag is not a physical answer.
  double best = -1.0;
  long best_lag = 0;
  std::vector<double> corr(static_cast<std::size_t>(limit) + 1, 0.0);

  for (long lag = 0; lag <= limit; ++lag) {
    double sum = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
      sum += r[static_cast<std::size_t>(lag) + i] * s[i];
    }
    corr[static_cast<std::size_t>(lag)] = sum;
    if (sum > best) {
      best = sum;
      best_lag = lag;
    }
  }

  if (best <= 0.0) return a;  // nothing correlated: silence, or the wrong signal entirely

  a.found = true;
  a.lag_samples = best_lag;
  a.lag_ms = static_cast<double>(best_lag) * 1000.0 / sample_rate;
  a.peak = best;

  // Second-best peak, excluding the main lobe around the winner: within that lobe the correlation
  // is still the SAME arrival sliding out of alignment, not a competing one.
  //
  // The lobe is narrow — a swept stimulus decorrelates within a few cycles of its lowest frequency,
  // not over its whole length. Using the full stimulus length as the guard (the obvious first
  // guess) is wrong: for any recording only somewhat longer than the stimulus it excludes the
  // entire searchable range, leaving second_peak at 0 and confidence permanently pinned at 1.
  // 1 ms is comfortably wider than the lobe at any frequency this method uses, and narrow enough
  // to leave real competing peaks visible.
  const long guard = std::max(1L, static_cast<long>(sample_rate / 1000.0));
  double second = 0.0;
  for (long lag = 0; lag <= limit; ++lag) {
    if (std::labs(lag - best_lag) <= guard) continue;
    second = std::max(second, corr[static_cast<std::size_t>(lag)]);
  }
  a.second_peak = second;
  return a;
}

}  // namespace nexus::measure
