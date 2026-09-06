#pragma once

#include <cstddef>
#include <vector>

#include "dsp/Biquad.h"

namespace nexus::dsp {

// Optional band-limiting for the driver: a high-pass to protect the woofer from sub-sonic energy
// and a low-pass to keep content within the driver's range. Second-order Butterworth sections,
// per-channel state. When disabled it passes audio through untouched. Interleaved float PCM, in
// place. (A full multi-way crossover with separate outputs is a later hardware concern; this
// single-band limiter covers the active-speaker case.)
class Crossover {
 public:
  Crossover(double sample_rate = 48000.0, int channels = 2) { configure(sample_rate, channels); }

  void configure(double sample_rate, int channels) {
    fs_ = sample_rate;
    channels_ = channels;
    hp_state_.assign(static_cast<std::size_t>(channels_), BiquadState{});
    lp_state_.assign(static_cast<std::size_t>(channels_), BiquadState{});
    rebuild();
  }

  void setHighPass(bool on, double fc) {
    hp_enabled_ = on;
    hp_fc_ = fc;
    rebuild();
  }
  void setLowPass(bool on, double fc) {
    lp_enabled_ = on;
    lp_fc_ = fc;
    rebuild();
  }
  void setEnabled(bool on) { enabled_ = on; }

  void process(float* io, std::size_t frames, int channels) {
    if (!enabled_ || (!hp_enabled_ && !lp_enabled_)) return;
    for (std::size_t f = 0; f < frames; ++f) {
      for (int ch = 0; ch < channels; ++ch) {
        const std::size_t idx =
            f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch);
        float x = io[idx];
        if (hp_enabled_) x = processSample(hp_, hp_state_[static_cast<std::size_t>(ch)], x);
        if (lp_enabled_) x = processSample(lp_, lp_state_[static_cast<std::size_t>(ch)], x);
        io[idx] = x;
      }
    }
  }

  void reset() {
    for (auto& s : hp_state_) s.reset();
    for (auto& s : lp_state_) s.reset();
  }

 private:
  void rebuild() {
    hp_ = butterworthHP2(hp_fc_, fs_);
    lp_ = butterworthLP2(lp_fc_, fs_);
  }

  double fs_ = 48000.0;
  int channels_ = 2;
  bool enabled_ = false;
  bool hp_enabled_ = false;
  bool lp_enabled_ = false;
  double hp_fc_ = 80.0;
  double lp_fc_ = 18000.0;
  BiquadCoeffs hp_, lp_;
  std::vector<BiquadState> hp_state_, lp_state_;
};

}  // namespace nexus::dsp
