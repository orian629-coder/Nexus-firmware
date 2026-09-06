#include "audio/AlsaAudioOutputHal.h"

#include <alsa/asoundlib.h>

#include "logging/Logger.h"

namespace nexus::audio {

using core::ErrorCode;
using core::Result;
using core::Status;

AlsaAudioOutputHal::AlsaAudioOutputHal(std::string device) : device_(std::move(device)) {}

AlsaAudioOutputHal::~AlsaAudioOutputHal() { close(); }

Status AlsaAudioOutputHal::open(int sample_rate, int channels) {
  channels_ = channels;
  NX_LOG_INFO("audio", "opening ALSA output device '" + device_ + "'");
  int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    pcm_ = nullptr;
    return Status::error(ErrorCode::IoError, "snd_pcm_open('" + device_ + "'): " +
                                                 snd_strerror(err));
  }

  // Interleaved 16-bit, config rate/channels, ~80ms buffer, ~20ms period.
  unsigned int rate = static_cast<unsigned int>(sample_rate);
  snd_pcm_uframes_t buffer_frames = static_cast<snd_pcm_uframes_t>(sample_rate) * 80 / 1000;
  snd_pcm_uframes_t period_frames = static_cast<snd_pcm_uframes_t>(sample_rate) * 20 / 1000;

  err = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           static_cast<unsigned int>(channels), rate,
                           /*soft_resample=*/1,
                           /*latency_us=*/static_cast<unsigned int>(buffer_frames * 1000000ULL /
                                                                    static_cast<unsigned>(sample_rate)));
  (void)period_frames;
  if (err < 0) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return Status::error(ErrorCode::IoError,
                         std::string("snd_pcm_set_params: ") + snd_strerror(err));
  }
  NX_LOG_INFO("audio", "alsa output open on " + device_ + " @ " + std::to_string(sample_rate) +
                           "Hz x" + std::to_string(channels));
  return Status::success();
}

Status AlsaAudioOutputHal::close() {
  if (pcm_) {
    snd_pcm_drain(pcm_);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
  return Status::success();
}

Result<std::size_t> AlsaAudioOutputHal::write(const std::int16_t* pcm, std::size_t frames) {
  if (!pcm_) return Status::error(ErrorCode::IoError, "output not open");
  snd_pcm_sframes_t written = snd_pcm_writei(pcm_, pcm, frames);
  if (written < 0) {
    // Recover from underrun/suspend and report zero frames for this block.
    written = snd_pcm_recover(pcm_, static_cast<int>(written), /*silent=*/1);
    if (written < 0) {
      return Status::error(ErrorCode::IoError,
                           std::string("snd_pcm_writei: ") + snd_strerror(static_cast<int>(written)));
    }
    return static_cast<std::size_t>(0);
  }
  return static_cast<std::size_t>(written);
}

}  // namespace nexus::audio
