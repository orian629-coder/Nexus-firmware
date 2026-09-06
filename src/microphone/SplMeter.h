#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace nexus::microphone {

// Computes sound pressure level (approximate dB SPL) from captured 16-bit PCM. The mic HAL is
// uncalibrated, so this reports a relative dBFS level plus an offset (`ref_db`) that acoustic
// calibration (Phase 7) tunes into true SPL. Pure and unit-testable.
class SplMeter {
 public:
  explicit SplMeter(double ref_db = 94.0) : ref_db_(ref_db) {}

  void setReferenceDb(double ref_db) { ref_db_ = ref_db; }

  // RMS level in dBFS (0 dBFS = full scale). Silence returns a large negative value.
  static double rmsDbfs(const std::vector<std::int16_t>& pcm) {
    if (pcm.empty()) return -120.0;
    double sum = 0.0;
    for (auto s : pcm) {
      const double x = static_cast<double>(s) / 32768.0;
      sum += x * x;
    }
    const double rms = std::sqrt(sum / static_cast<double>(pcm.size()));
    return 20.0 * std::log10(rms + 1e-12);
  }

  // Approximate SPL = dBFS + reference offset.
  double splDb(const std::vector<std::int16_t>& pcm) const { return rmsDbfs(pcm) + ref_db_; }

 private:
  double ref_db_;
};

}  // namespace nexus::microphone
