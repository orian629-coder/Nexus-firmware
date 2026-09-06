#include "audio/PlaybackManager.h"

#include <algorithm>

namespace nexus::audio {

PlaybackManager::PlaybackManager(AudioBuffer* buffer, StreamSync* sync, IAudioOutputHal* output,
                                 config::ConfigManager* config, Clock clock)
    : buffer_(buffer), sync_(sync), output_(output), config_(config), clock_(std::move(clock)) {}

core::Status PlaybackManager::open(int sample_rate, int channels) {
  return output_->open(sample_rate, channels);
}

core::Status PlaybackManager::close() {
  started_ = false;
  return output_->close();
}

void PlaybackManager::applyGain(std::vector<std::int16_t>& pcm) const {
  const auto& cfg = config_->get();
  if (cfg.audio.muted) {
    std::fill(pcm.begin(), pcm.end(), std::int16_t{0});
    return;
  }
  const int vol = std::clamp(cfg.audio.volume, 0, 100);
  if (vol == 100) return;
  const double gain = vol / 100.0;
  for (auto& s : pcm) {
    s = static_cast<std::int16_t>(std::clamp(static_cast<double>(s) * gain, -32768.0, 32767.0));
  }
}

std::size_t PlaybackManager::step() {
  // Wait for prefill before the first packet, then keep playing until underflow.
  if (!started_) {
    if (!buffer_->ready()) return 0;
    started_ = true;
  }

  // Release the packet at its intended wall-clock moment, not as soon as it arrives.
  //
  // This is what makes multi-room alignment possible: every speaker receives the same absolute
  // timestamp and waits for its own (NTP-synced) clock to reach it, so they start the same sample
  // together regardless of who received it first. Playing on arrival — the previous behaviour —
  // left each speaker running on its own network jitter, which no amount of buffering can align.
  //
  // The check happens BEFORE popping, via peekTimestamp(): a packet that came out of the queue
  // early cannot be put back. Returning 0 makes the caller write a silence block, which is the
  // correct thing to emit while waiting and keeps the output device from starving.
  //
  // A timestamp of 0 means the sender did not stamp this packet (older streamer, or a test source),
  // so it is played immediately rather than being held forever waiting for a moment that never
  // comes.
  // `clock_` is checked too: it is a std::function and an empty one would throw on the audio
  // thread. Without a clock there is no way to know whether a packet is due, so the only safe
  // behaviour is the old one — play on arrival, unsynchronised but not silent.
  if (sync_ && clock_) {
    const auto ts = buffer_->peekTimestamp();
    if (ts && *ts > 0.0 && !sync_->due(*ts, clock_())) {
      // Safety valve: never hold so long that the buffer overflows.
      //
      // Waiting only works while the queue can store the audio being held. If the sender's lead
      // exceeds what the buffer can hold (capacity x block duration), holding drops packets at the
      // TAIL as overflow while still waiting on the head — the stream is destroyed to preserve a
      // schedule it can no longer meet. Measured on hardware: a 2 s lead against a 0.5 s buffer
      // dropped 3779 packets and never played a sample.
      //
      // Playing early loses alignment, which is recoverable; dropping the audio is not. So once the
      // queue is nearly full, release regardless of schedule and let StreamSync converge.
      if (!buffer_->nearlyFull()) {
        return 0;  // not yet — the caller emits silence and tries again
      }
    }
  }

  auto pkt = buffer_->pop();
  if (!pkt) {
    started_ = false;  // underflow: re-prefill before resuming
    return 0;
  }

  const std::size_t frames =
      pkt->header.frame_count ? pkt->header.frame_count : (pkt->samples.size() / 2);
  const int channels = frames ? static_cast<int>(pkt->samples.size() / frames) : 2;

  // DSP chain (EQ/crossover/compressor/limiter/delay/gain) runs first, then the volume/mute
  // stage. The DSP hook is optional so audio stays decoupled from the dsp module.
  if (dsp_hook_) dsp_hook_(pkt->samples.data(), frames, channels);
  applyGain(pkt->samples);

  auto w = output_->write(pkt->samples.data(), frames);
  if (w.ok()) frames_played_ += w.value();
  return w.ok() ? w.value() : 0;
}

}  // namespace nexus::audio
