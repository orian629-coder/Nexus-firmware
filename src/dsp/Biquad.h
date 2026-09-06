#pragma once

#include <cmath>

// Second-order IIR (biquad) filter for real-time audio. Math from the RBJ "Audio EQ Cookbook":
// peaking (parametric) EQ + second-order Butterworth high/low-pass, processed in Direct Form II
// Transposed (numerically stable, small state). Header-only, no dependencies — used by the DSP
// core and by tests. Ported (comments translated) from the prior speaker-app src/dsp/Biquad.h.

namespace nexus::dsp {

// Normalized biquad coefficients (a0 == 1 after normalization).
struct BiquadCoeffs {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0;  // numerator
  double a1 = 0.0, a2 = 0.0;            // denominator (a0 normalized to 1)
};

// Per-filter state (Direct Form II Transposed).
struct BiquadState {
  double z1 = 0.0, z2 = 0.0;
  void reset() { z1 = 0.0; z2 = 0.0; }
};

// Peaking (parametric) EQ: boost/cut around fc with bandwidth set by Q. gainDb > 0 boosts.
inline BiquadCoeffs peakingEq(double fc, double gainDb, double q, double fs) {
  double A = std::pow(10.0, gainDb / 40.0);
  double w0 = 2.0 * M_PI * fc / fs;
  double cosw0 = std::cos(w0);
  double alpha = std::sin(w0) / (2.0 * q);

  double b0 = 1.0 + alpha * A;
  double b1 = -2.0 * cosw0;
  double b2 = 1.0 - alpha * A;
  double a0 = 1.0 + alpha / A;
  double a1 = -2.0 * cosw0;
  double a2 = 1.0 - alpha / A;
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Second-order Butterworth high-pass (Q = 1/sqrt(2)). Cuts below fc.
inline BiquadCoeffs butterworthHP2(double fc, double fs) {
  double w0 = 2.0 * M_PI * fc / fs;
  double cosw0 = std::cos(w0);
  double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);

  double b0 = (1.0 + cosw0) / 2.0;
  double b1 = -(1.0 + cosw0);
  double b2 = (1.0 + cosw0) / 2.0;
  double a0 = 1.0 + alpha;
  double a1 = -2.0 * cosw0;
  double a2 = 1.0 - alpha;
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Second-order Butterworth low-pass (Q = 1/sqrt(2)). Cuts above fc.
inline BiquadCoeffs butterworthLP2(double fc, double fs) {
  double w0 = 2.0 * M_PI * fc / fs;
  double cosw0 = std::cos(w0);
  double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);

  double b0 = (1.0 - cosw0) / 2.0;
  double b1 = 1.0 - cosw0;
  double b2 = (1.0 - cosw0) / 2.0;
  double a0 = 1.0 + alpha;
  double a1 = -2.0 * cosw0;
  double a2 = 1.0 - alpha;
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Process one sample through a biquad (Direct Form II Transposed), updating state.
inline float processSample(const BiquadCoeffs& c, BiquadState& s, float x) {
  double y = c.b0 * x + s.z1;
  s.z1 = c.b1 * x - c.a1 * y + s.z2;
  s.z2 = c.b2 * x - c.a2 * y;
  return static_cast<float>(y);
}

// Magnitude response (dB) of a filter at `freq` — for tests and display.
inline double magnitudeDb(const BiquadCoeffs& c, double freq, double fs) {
  double w = 2.0 * M_PI * freq / fs;
  double cosw = std::cos(w), sinw = std::sin(w);
  double cos2w = std::cos(2 * w), sin2w = std::sin(2 * w);
  double numRe = c.b0 + c.b1 * cosw + c.b2 * cos2w;
  double numIm = -(c.b1 * sinw + c.b2 * sin2w);
  double denRe = 1.0 + c.a1 * cosw + c.a2 * cos2w;
  double denIm = -(c.a1 * sinw + c.a2 * sin2w);
  double num = std::sqrt(numRe * numRe + numIm * numIm);
  double den = std::sqrt(denRe * denRe + denIm * denIm);
  return 20.0 * std::log10((num / den) + 1e-20);
}

}  // namespace nexus::dsp
