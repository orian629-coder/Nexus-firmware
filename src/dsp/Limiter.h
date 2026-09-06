#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nexus::dsp {

// Peak limiter with attack/release envelope. Prevents the signal from exceeding the threshold,
// protecting the amplifier/driver from clipping. Operates on the linked (max across channels)
// level so the stereo image is preserved. Interleaved float PCM, in place.
class Limiter {
 public:
  Limiter(double sample_rate = 48000.0) : fs_(sample_rate) { recompute(); }

  void setSampleRate(double fs) {
    fs_ = fs;
    recompute();
  }
  void setThresholdDb(double db) { threshold_ = std::pow(10.0, db / 20.0); }
  void setAttackMs(double ms) {
    attack_ms_ = ms;
    recompute();
  }
  void setReleaseMs(double ms) {
    release_ms_ = ms;
    recompute();
  }
  void setEnabled(bool on) { enabled_ = on; }

  void process(float* io, std::size_t frames, int channels) {
    if (!enabled_) return;
    for (std::size_t f = 0; f < frames; ++f) {
      // Peak across channels for this frame.
      float peak = 0.0f;
      for (int ch = 0; ch < channels; ++ch) {
        peak = std::max(peak, std::fabs(io[f * static_cast<std::size_t>(channels) +
                                          static_cast<std::size_t>(ch)]));
      }
      // Desired gain to keep the peak at/under threshold.
      double desired = (peak > threshold_) ? (threshold_ / (peak + 1e-9)) : 1.0;
      // Envelope: fast attack (gain down), slow release (gain up).
      if (desired < gain_)
        gain_ += (desired - gain_) * attack_coeff_;
      else
        gain_ += (desired - gain_) * release_coeff_;
      for (int ch = 0; ch < channels; ++ch) {
        io[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)] *=
            static_cast<float>(gain_);
      }
    }
  }

  void reset() { gain_ = 1.0; }

 private:
  void recompute() {
    attack_coeff_ = 1.0 - std::exp(-1.0 / (std::max(0.01, attack_ms_) * 0.001 * fs_));
    release_coeff_ = 1.0 - std::exp(-1.0 / (std::max(0.01, release_ms_) * 0.001 * fs_));
  }

  double fs_;
  bool enabled_ = true;
  double threshold_ = std::pow(10.0, -1.0 / 20.0);  // -1 dBFS default
  double attack_ms_ = 1.0;
  double release_ms_ = 50.0;
  double attack_coeff_ = 0.5;
  double release_coeff_ = 0.01;
  double gain_ = 1.0;
};

}  // namespace nexus::dsp
