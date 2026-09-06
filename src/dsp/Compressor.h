#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nexus::dsp {

// Dynamic range compressor: above the threshold the signal is attenuated by `ratio`, with
// attack/release smoothing and a makeup gain. Uses the linked peak across channels so the stereo
// image is preserved. Interleaved float PCM, in place.
class Compressor {
 public:
  Compressor(double sample_rate = 48000.0) : fs_(sample_rate) { recompute(); }

  void setSampleRate(double fs) {
    fs_ = fs;
    recompute();
  }
  void setThresholdDb(double db) { threshold_db_ = db; }
  void setRatio(double r) { ratio_ = std::max(1.0, r); }
  void setAttackMs(double ms) {
    attack_ms_ = ms;
    recompute();
  }
  void setReleaseMs(double ms) {
    release_ms_ = ms;
    recompute();
  }
  void setMakeupGainDb(double db) { makeup_ = std::pow(10.0, db / 20.0); }
  void setEnabled(bool on) { enabled_ = on; }

  void process(float* io, std::size_t frames, int channels) {
    if (!enabled_) return;
    for (std::size_t f = 0; f < frames; ++f) {
      float peak = 0.0f;
      for (int ch = 0; ch < channels; ++ch) {
        peak = std::max(peak, std::fabs(io[f * static_cast<std::size_t>(channels) +
                                          static_cast<std::size_t>(ch)]));
      }
      const double level_db = 20.0 * std::log10(peak + 1e-9);
      double target_gain_db = 0.0;
      if (level_db > threshold_db_) {
        const double over = level_db - threshold_db_;
        target_gain_db = -(over - over / ratio_);  // reduce the amount over threshold by ratio
      }
      const double target_gain = std::pow(10.0, target_gain_db / 20.0);
      const double coeff = target_gain < env_ ? attack_coeff_ : release_coeff_;
      env_ += (target_gain - env_) * coeff;
      const float g = static_cast<float>(env_ * makeup_);
      for (int ch = 0; ch < channels; ++ch) {
        io[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)] *= g;
      }
    }
  }

  void reset() { env_ = 1.0; }

 private:
  void recompute() {
    attack_coeff_ = 1.0 - std::exp(-1.0 / (std::max(0.01, attack_ms_) * 0.001 * fs_));
    release_coeff_ = 1.0 - std::exp(-1.0 / (std::max(0.01, release_ms_) * 0.001 * fs_));
  }

  double fs_;
  bool enabled_ = false;  // off by default; enabled via config
  double threshold_db_ = -18.0;
  double ratio_ = 2.0;
  double attack_ms_ = 10.0;
  double release_ms_ = 100.0;
  double makeup_ = 1.0;
  double attack_coeff_ = 0.1;
  double release_coeff_ = 0.01;
  double env_ = 1.0;
};

}  // namespace nexus::dsp
