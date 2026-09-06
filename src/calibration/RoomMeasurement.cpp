#include "calibration/RoomMeasurement.h"

#include <cmath>

#include "calibration/Fft.h"

namespace nexus::calibration {

std::vector<double> RoomMeasurement::toDouble(const std::vector<std::int16_t>& pcm) {
  std::vector<double> out(pcm.size());
  for (std::size_t i = 0; i < pcm.size(); ++i) out[i] = static_cast<double>(pcm[i]) / 32768.0;
  return out;
}

std::array<double, dsp::kEqBands> RoomMeasurement::bandEnergyDb(
    const std::vector<double>& samples) const {
  const auto mag = magnitudeSpectrum(samples, fft_size_);
  const double bin_hz = fs_ / static_cast<double>(fft_size_);

  std::array<double, dsp::kEqBands> out{};
  for (int b = 0; b < dsp::kEqBands; ++b) {
    const double fc = dsp::kBandFreqs[b];
    // 1/3-octave-ish neighborhood around the center frequency.
    const double lo = fc / 1.12;
    const double hi = fc * 1.12;
    const std::size_t k_lo = static_cast<std::size_t>(std::max(0.0, lo / bin_hz));
    const std::size_t k_hi =
        std::min(mag.size() - 1, static_cast<std::size_t>(hi / bin_hz) + 1);

    double energy = 0.0;
    std::size_t count = 0;
    for (std::size_t k = k_lo; k <= k_hi && k < mag.size(); ++k) {
      energy += mag[k] * mag[k];
      ++count;
    }
    const double mean = count ? energy / static_cast<double>(count) : 1e-12;
    out[b] = 10.0 * std::log10(mean + 1e-12);
  }
  return out;
}

}  // namespace nexus::calibration
