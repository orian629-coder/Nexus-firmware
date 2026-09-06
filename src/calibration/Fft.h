#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace nexus::calibration {

// Self-contained iterative radix-2 Cooley-Tukey FFT — no external dependency (the prior
// speaker-app used FFTW; a pure implementation keeps calibration testable off-target and avoids a
// heavy build dep). `n` must be a power of two.
inline void fftRadix2(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  if (n <= 1) return;

  // Bit-reversal permutation.
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }

  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * M_PI / static_cast<double>(len);
    const std::complex<double> wlen(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        std::complex<double> u = a[i + k];
        std::complex<double> v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

// Compute the magnitude spectrum of a real signal (Hann-windowed). Returns n/2+1 magnitudes;
// bin k corresponds to frequency k * fs / n. `samples` is truncated/zero-padded to `fft_size`
// (power of two).
inline std::vector<double> magnitudeSpectrum(const std::vector<double>& samples,
                                             std::size_t fft_size) {
  std::vector<std::complex<double>> buf(fft_size, {0.0, 0.0});
  const std::size_t m = std::min(samples.size(), fft_size);
  for (std::size_t i = 0; i < m; ++i) {
    // Hann window to reduce spectral leakage.
    const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                          static_cast<double>(fft_size - 1));
    buf[i] = {samples[i] * w, 0.0};
  }
  fftRadix2(buf);
  std::vector<double> mag(fft_size / 2 + 1);
  for (std::size_t k = 0; k < mag.size(); ++k) mag[k] = std::abs(buf[k]);
  return mag;
}

}  // namespace nexus::calibration
