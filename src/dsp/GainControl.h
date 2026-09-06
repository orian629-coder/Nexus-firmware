#pragma once

#include <cmath>
#include <cstddef>

namespace nexus::dsp {

// Simple gain stage in dB, applied to interleaved float PCM in place. Used for both the input and
// output gain stages of the DSP chain. Gain is smoothed per-sample toward the target to avoid
// zipper noise when the level changes.
class GainControl {
 public:
  explicit GainControl(double gain_db = 0.0) { setGainDb(gain_db); }

  void setGainDb(double db) { target_ = std::pow(10.0, db / 20.0); }
  double gainLinear() const { return target_; }

  void process(float* io, std::size_t frames, int channels) {
    const double step = 0.001;  // smoothing coefficient
    for (std::size_t f = 0; f < frames; ++f) {
      current_ += (target_ - current_) * step;
      for (int ch = 0; ch < channels; ++ch) {
        io[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)] *=
            static_cast<float>(current_);
      }
    }
  }

  void reset() { current_ = target_; }

 private:
  double target_ = 1.0;
  double current_ = 1.0;
};

}  // namespace nexus::dsp
