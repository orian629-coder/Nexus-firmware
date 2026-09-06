#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/Result.h"
#include "sources/IAudioSource.h"

namespace nexus::streamer::sources {

// Plays a RIFF/WAVE file. This is the first source that can produce audio on its own — until now
// the streamer could only read raw PCM from stdin, so playing a file meant piping ffmpeg into it and
// nothing could be started from the UI.
//
// Scope is deliberately narrow: uncompressed 16-bit PCM, which is what `format 1` WAV files carry.
// Anything else (FLAC, MP3, 24-bit) goes through ProcessPcmSource, which shells out to ffmpeg. Two
// conversions happen here because they are cheap and cover the common cases:
//   - mono → stereo by duplicating the channel
//   - any sample rate → 48 kHz by linear interpolation
// Linear resampling is audibly imperfect for large ratios; a proper SRC is out of scope and the
// wire format is fixed at 48 kHz, so this is the honest minimum rather than a quality claim.
class WavFileSource : public IAudioSource {
 public:
  ~WavFileSource() override;

  // Open and parse the header. Returns an error for a missing file, a non-PCM codec, or an
  // unsupported bit depth — all of which would otherwise surface as noise rather than a message.
  static core::Result<std::unique_ptr<WavFileSource>> open(const std::string& path);

  std::size_t read(std::size_t max_frames, std::vector<std::int16_t>& out) override;
  bool exhausted() const override { return exhausted_; }

  int sampleRate() const override { return 48000; }  // always converted to the wire rate
  int channels() const override { return 2; }        // always converted to stereo
  bool live() const override { return false; }

  int sourceSampleRate() const { return src_rate_; }
  int sourceChannels() const { return src_channels_; }

 private:
  WavFileSource() = default;

  // Pull `frames` source-rate frames, already widened to stereo int16.
  std::size_t readSourceFrames(std::size_t frames, std::vector<std::int16_t>& out);

  std::FILE* file_ = nullptr;
  int src_rate_ = 48000;
  int src_channels_ = 2;
  int bits_ = 16;
  std::uint64_t data_remaining_ = 0;  // bytes left in the data chunk
  bool exhausted_ = false;

  // Resampler state: position within the source stream, in fractional source frames.
  double pos_ = 0.0;
  std::vector<std::int16_t> pending_;  // decoded stereo source frames not yet consumed
  bool source_eof_ = false;
};

}  // namespace nexus::streamer::sources
