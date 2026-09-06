#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace nexus::dsp {

// Per-speaker playback delay (milliseconds), used to time-align speakers at different distances.
// Implemented as a per-channel ring buffer. Processes interleaved float PCM in place.
class Delay {
 public:
  Delay(double sample_rate = 48000.0, int channels = 2) { configure(sample_rate, channels); }

  void configure(double sample_rate, int channels) {
    fs_ = sample_rate;
    channels_ = channels;
    setDelayMs(delay_ms_);
  }

  void setDelayMs(double ms) {
    delay_ms_ = ms < 0 ? 0 : ms;
    const std::size_t samples =
        static_cast<std::size_t>((delay_ms_ / 1000.0) * fs_) * static_cast<std::size_t>(channels_);
    buffer_.assign(samples, 0.0f);
    pos_ = 0;
  }

  double delayMs() const { return delay_ms_; }

  void process(float* io, std::size_t frames) {
    if (buffer_.empty()) return;  // no delay configured
    const std::size_t n = frames * static_cast<std::size_t>(channels_);
    for (std::size_t i = 0; i < n; ++i) {
      float delayed = buffer_[pos_];
      buffer_[pos_] = io[i];
      io[i] = delayed;
      pos_ = (pos_ + 1) % buffer_.size();
    }
  }

  void reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    pos_ = 0;
  }

 private:
  double fs_ = 48000.0;
  int channels_ = 2;
  double delay_ms_ = 0.0;
  std::vector<float> buffer_;
  std::size_t pos_ = 0;
};

}  // namespace nexus::dsp
