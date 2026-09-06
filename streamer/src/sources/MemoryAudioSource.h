#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "sources/IAudioSource.h"

namespace nexus::streamer::sources {

// A finite in-memory PCM source (interleaved int16 stereo @48k). Used to drive the pipeline in
// tests and as the backing for a simple file/tone player before real capture sources land. This is
// the streamer analogue of the speaker's StubAudioSource: pure, deterministic, no OS dependency.
class MemoryAudioSource : public IAudioSource {
 public:
  // `samples` is channels*frames interleaved int16.
  explicit MemoryAudioSource(std::vector<std::int16_t> samples, int channels = 2)
      : samples_(std::move(samples)), channels_(channels) {}

  std::size_t read(std::size_t max_frames, std::vector<std::int16_t>& out) override {
    const std::size_t total_frames = samples_.size() / static_cast<std::size_t>(channels_);
    const std::size_t remaining = total_frames - pos_frames_;
    const std::size_t n = std::min(max_frames, remaining);
    const std::size_t begin = pos_frames_ * static_cast<std::size_t>(channels_);
    const std::size_t count = n * static_cast<std::size_t>(channels_);
    out.insert(out.end(), samples_.begin() + begin, samples_.begin() + begin + count);
    pos_frames_ += n;
    return n;
  }

  bool exhausted() const override {
    return pos_frames_ >= samples_.size() / static_cast<std::size_t>(channels_);
  }

 private:
  std::vector<std::int16_t> samples_;
  int channels_;
  std::size_t pos_frames_ = 0;
};

}  // namespace nexus::streamer::sources
