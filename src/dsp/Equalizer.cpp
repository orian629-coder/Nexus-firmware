#include "dsp/Equalizer.h"

#include <algorithm>

namespace nexus::dsp {

namespace {
constexpr double kBandQ = 4.3;  // ~1/3-octave bands
}

Equalizer::Equalizer(double sample_rate, int channels) : fs_(sample_rate), channels_(channels) {
  gains_db_.fill(0.0);
  setChannels(channels);
  rebuild();
}

void Equalizer::setChannels(int channels) {
  channels_ = std::max(1, channels);
  state_.assign(static_cast<std::size_t>(channels_), std::array<BiquadState, kEqBands>{});
}

void Equalizer::setSampleRate(double fs) {
  fs_ = fs;
  rebuild();
}

void Equalizer::setGains(const std::array<double, kEqBands>& gains_db) {
  for (int i = 0; i < kEqBands; ++i) gains_db_[i] = std::clamp(gains_db[i], -10.0, 10.0);
  rebuild();
}

void Equalizer::setBandGain(int band, double gain_db) {
  if (band < 0 || band >= kEqBands) return;
  gains_db_[band] = std::clamp(gain_db, -10.0, 10.0);
  rebuild();
}

void Equalizer::rebuild() {
  for (int i = 0; i < kEqBands; ++i) {
    coeffs_[i] = peakingEq(kBandFreqs[i], gains_db_[i], kBandQ, fs_);
  }
}

void Equalizer::process(float* io, std::size_t frames) {
  for (std::size_t f = 0; f < frames; ++f) {
    for (int ch = 0; ch < channels_; ++ch) {
      float x = io[f * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(ch)];
      auto& st = state_[static_cast<std::size_t>(ch)];
      for (int b = 0; b < kEqBands; ++b) {
        x = processSample(coeffs_[b], st[b], x);
      }
      io[f * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(ch)] = x;
    }
  }
}

void Equalizer::reset() {
  for (auto& ch : state_) {
    for (auto& s : ch) s.reset();
  }
}

double Equalizer::magnitudeDb(double freq) const {
  double sum = 0.0;
  for (int b = 0; b < kEqBands; ++b) sum += nexus::dsp::magnitudeDb(coeffs_[b], freq, fs_);
  return sum;
}

}  // namespace nexus::dsp
