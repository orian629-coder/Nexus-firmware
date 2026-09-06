#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "audio/AudioBuffer.h"
#include "audio/IAudioOutputHal.h"
#include "audio/StreamSync.h"
#include "config/ConfigManager.h"

namespace nexus::audio {

// Pulls packets from the jitter buffer at their scheduled playback time, applies volume/mute from
// config, and writes PCM to the audio output HAL. The scheduling/gain logic is pure and testable;
// the actual output device is behind IAudioOutputHal (stub off-target, ALSA/I2S on the Pi).
//
// Phase 4 provides the core step() that the receiver/playback loop drives; DSP (Phase 5) inserts
// between the buffer and the output. Volume is applied as a simple linear scale here until the DSP
// gain stage takes over.
class PlaybackManager {
 public:
  using Clock = std::function<double()>;  // epoch seconds

  // Optional DSP hook applied to each block before output — keeps audio decoupled from the dsp
  // module. Signature: (interleaved int16 PCM, frames, channels).
  using DspHook = std::function<void(std::int16_t*, std::size_t, int)>;

  PlaybackManager(AudioBuffer* buffer, StreamSync* sync, IAudioOutputHal* output,
                  config::ConfigManager* config, Clock clock);

  void setDspHook(DspHook hook) { dsp_hook_ = std::move(hook); }

  // Open the output device for the stream format.
  core::Status open(int sample_rate, int channels);
  core::Status close();

  // Play one due packet if the buffer is ready and the next packet's time has arrived. Returns the
  // number of frames written (0 if nothing was due / buffer not ready / muted-with-nothing).
  std::size_t step();

  std::uint64_t framesPlayed() const { return frames_played_; }

 private:
  // Apply the current volume (0..100) as a linear gain to interleaved 16-bit PCM in place.
  void applyGain(std::vector<std::int16_t>& pcm) const;

  AudioBuffer* buffer_;
  StreamSync* sync_;
  IAudioOutputHal* output_;
  config::ConfigManager* config_;
  Clock clock_;
  DspHook dsp_hook_;
  bool started_ = false;             // began playback (prefill satisfied)
  std::uint64_t frames_played_ = 0;
};

}  // namespace nexus::audio
