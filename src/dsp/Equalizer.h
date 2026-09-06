#pragma once

#include <array>
#include <vector>

#include "dsp/Biquad.h"

namespace nexus::dsp {

// 32 standard ISO center frequencies (30 Hz – 20 kHz), per the spec's 32-band EQ. Extended from
// the prior 30-band table.
constexpr int kEqBands = 32;
constexpr std::array<double, kEqBands> kBandFreqs = {
    30.0,    40.0,    50.0,    63.0,    80.0,    100.0,   125.0,   160.0,
    200.0,   250.0,   315.0,   400.0,   500.0,   630.0,   800.0,   1000.0,
    1250.0,  1600.0,  2000.0,  2500.0,  3150.0,  4000.0,  5000.0,  6300.0,
    8000.0,  10000.0, 12500.0, 14000.0, 16000.0, 18000.0, 19000.0, 20000.0};

// 32-band parametric equalizer. Each band is a peaking filter at an ISO center frequency; per-band
// gain is settable in the range -10..+10 dB (per spec). Processes interleaved float PCM in place,
// keeping independent biquad state per channel so stereo images aren't smeared.
class Equalizer {
 public:
  explicit Equalizer(double sample_rate = 48000.0, int channels = 2);

  // Set per-band gains in dB. Values are clamped to [-10, +10]. Rebuilds coefficients.
  void setGains(const std::array<double, kEqBands>& gains_db);
  void setBandGain(int band, double gain_db);
  const std::array<double, kEqBands>& gains() const { return gains_db_; }

  void setChannels(int channels);
  void setSampleRate(double fs);

  // Process a block of interleaved float samples in place (frames * channels).
  void process(float* io, std::size_t frames);

  void reset();

  // Combined magnitude response (dB) of all bands at `freq` — for tests/verification.
  double magnitudeDb(double freq) const;

 private:
  void rebuild();

  double fs_;
  int channels_;
  std::array<double, kEqBands> gains_db_{};
  std::array<BiquadCoeffs, kEqBands> coeffs_;
  // state_[ch][band]
  std::vector<std::array<BiquadState, kEqBands>> state_;
};

}  // namespace nexus::dsp
