#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nexus::streamer::sources {

// Capture-side abstraction: a source of interleaved int16 stereo PCM at 48 kHz (the wire format).
// Real sources (ALSA/PipeWire capture, A2DP, AirPlay, Spotify) implement this; a file/stub source
// implements it for off-target tests, mirroring the speaker's IAudioSource + StubAudioSource split.
class IAudioSource {
 public:
  virtual ~IAudioSource() = default;

  // Fill up to `max_frames` frames (a frame = channels interleaved samples) into `out`, appending
  // channels*frames int16 samples. Returns the number of FRAMES produced; 0 means end-of-stream
  // (for finite sources like a file) or "no data available yet" (for live sources — the caller
  // paces itself). Never blocks longer than one capture period.
  virtual std::size_t read(std::size_t max_frames, std::vector<std::int16_t>& out) = 0;

  // True once a finite source has been fully consumed. Live sources always return false.
  virtual bool exhausted() const = 0;

  // Native format of this source. Defaulted to the wire format so existing sources are unchanged;
  // a file source reports what it actually found and converts on read().
  virtual int sampleRate() const { return 48000; }
  virtual int channels() const { return 2; }

  // True for a source with no end (capture, Bluetooth, AirPlay). It changes what read()==0 means:
  // for a finite source that is end-of-stream, but for a live one it only means "nothing available
  // right now" and the stream must stay open. Getting this wrong ends playback on the first
  // underrun, which is exactly what a live source does constantly.
  virtual bool live() const { return false; }
};

}  // namespace nexus::streamer::sources
