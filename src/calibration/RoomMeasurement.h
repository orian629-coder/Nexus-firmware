#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dsp/Equalizer.h"  // kEqBands + kBandFreqs

namespace nexus::calibration {

// Analyzes a microphone capture into per-band energy at the 32 EQ center frequencies, using the
// FFT. This is the "how the room sounds" measurement that AutoEq inverts into a correction curve.
// Pure functions — fully unit-testable with synthetic signals.
class RoomMeasurement {
 public:
  explicit RoomMeasurement(double sample_rate = 48000.0, std::size_t fft_size = 8192)
      : fs_(sample_rate), fft_size_(fft_size) {}

  // Convert 16-bit PCM to normalized doubles [-1, 1].
  static std::vector<double> toDouble(const std::vector<std::int16_t>& pcm);

  // Per-band energy in dB (relative), one value per EQ band. Averages the magnitude spectrum over
  // each band's frequency neighborhood.
  std::array<double, dsp::kEqBands> bandEnergyDb(const std::vector<double>& samples) const;

  double sampleRate() const { return fs_; }

 private:
  double fs_;
  std::size_t fft_size_;
};

}  // namespace nexus::calibration
