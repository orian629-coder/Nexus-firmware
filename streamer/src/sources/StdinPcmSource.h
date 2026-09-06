#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sources/IAudioSource.h"

namespace nexus::streamer::sources {

// Live capture source: reads raw interleaved int16 stereo PCM @48k from a FILE* (stdin by default).
// This is the platform-agnostic capture path — anything that can emit raw PCM feeds it:
//   • macOS system audio via BlackHole:  ffmpeg -f avfoundation -i ":BlackHole 2ch" -ar 48000 \
//         -ac 2 -f s16le - | nexus-streamer --stream <speaker-ip>
//   • a WAV/file:                        ffmpeg -i song.wav -ar 48000 -ac 2 -f s16le - | ...
//   • Pi PipeWire capture:               pw-record --rate 48000 --channels 2 --format s16 - | ...
// Mirrors AOA's BlackHole capture (sounddevice input), but instead of routing locally it feeds the
// timestamped-UDP sender so audio actually goes over the network to multiple speakers.
//
// read() blocks until it has data or EOF (upstream closed). Returns 0 frames at EOF; the paced
// sender treats that as end-of-stream. A short final read returns the partial frame count.
class StdinPcmSource : public IAudioSource {
 public:
  explicit StdinPcmSource(std::FILE* in, int channels = 2) : in_(in), channels_(channels) {}

  std::size_t read(std::size_t max_frames, std::vector<std::int16_t>& out) override {
    const std::size_t want_samples = max_frames * static_cast<std::size_t>(channels_);
    const std::size_t base = out.size();
    out.resize(base + want_samples);
    const std::size_t got =
        std::fread(out.data() + base, sizeof(std::int16_t), want_samples, in_);
    // Trim to whole frames; drop any trailing partial sample (channel-misaligned tail).
    const std::size_t frames = got / static_cast<std::size_t>(channels_);
    out.resize(base + frames * static_cast<std::size_t>(channels_));
    if (got == 0) eof_ = true;
    return frames;
  }

  bool exhausted() const override { return eof_; }

  // Whatever is piped in is a live stream from this side's point of view — the streamer has no idea
  // whether upstream is a capture device or a file being decoded, and cannot seek or restart it.
  bool live() const override { return true; }

 private:
  std::FILE* in_;
  int channels_;
  bool eof_ = false;
};

}  // namespace nexus::streamer::sources
