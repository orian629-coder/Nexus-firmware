#pragma once

#include <cstdint>
#include <vector>

#include "core/Result.h"

namespace nexus::microphone {

// Hardware abstraction for the measurement microphone. Stub vs ALSA-backed real impl selected by
// NEXUS_STUB_HAL. Used only for measurement/calibration — the mic never streams audio out.
class IMicrophoneHal {
 public:
  virtual ~IMicrophoneHal() = default;
  virtual core::Status open() = 0;
  virtual core::Status close() = 0;
  // Capture `frames` mono 16-bit PCM samples.
  virtual core::Result<std::vector<std::int16_t>> capture(std::size_t frames) = 0;
};

// Dev-host stub. Generates a deterministic low-level pseudo-noise signal so SPL / noise / self-test
// logic is exercised off-target. `amplitude` and `fail` are settable by tests.
class StubMicrophoneHal : public IMicrophoneHal {
 public:
  core::Status open() override { return core::Status::success(); }
  core::Status close() override { return core::Status::success(); }
  core::Result<std::vector<std::int16_t>> capture(std::size_t frames) override {
    if (fail) return core::Status::error(core::ErrorCode::IoError, "mic capture failed");
    std::vector<std::int16_t> pcm(frames);
    // Deterministic LCG-based pseudo-noise (avoids argless rand); scaled to `amplitude`.
    std::uint32_t s = seed_;
    for (std::size_t i = 0; i < frames; ++i) {
      s = s * 1664525u + 1013904223u;
      const double n = (static_cast<double>(s >> 16) / 32768.0) - 1.0;  // [-1, 1)
      pcm[i] = static_cast<std::int16_t>(n * amplitude);
    }
    seed_ = s;
    return pcm;
  }

  double amplitude = 300.0;  // small ambient level by default
  bool fail = false;

 private:
  std::uint32_t seed_ = 12345u;
};

}  // namespace nexus::microphone
