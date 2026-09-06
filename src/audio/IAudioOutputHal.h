#pragma once

#include <cstddef>
#include <cstdint>

#include "core/Result.h"

namespace nexus::audio {

// Hardware abstraction for the audio output (ALSA/I2S sink on the Pi). Stub vs real impl
// selected by NEXUS_STUB_HAL so the audio module builds and tests off-target.
class IAudioOutputHal {
 public:
  virtual ~IAudioOutputHal() = default;
  virtual core::Status open(int sample_rate, int channels) = 0;
  virtual core::Status close() = 0;
  // Write interleaved 16-bit PCM; returns frames written.
  virtual core::Result<std::size_t> write(const std::int16_t* pcm, std::size_t frames) = 0;
};

class StubAudioOutputHal : public IAudioOutputHal {
 public:
  core::Status open(int, int) override { return core::Status::success(); }
  core::Status close() override { return core::Status::success(); }
  core::Result<std::size_t> write(const std::int16_t*, std::size_t frames) override {
    return frames;  // pretend everything was consumed
  }
};

}  // namespace nexus::audio
