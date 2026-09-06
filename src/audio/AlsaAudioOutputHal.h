#pragma once

#include <string>

#include "audio/IAudioOutputHal.h"

// Forward-declare the ALSA PCM handle so the header doesn't pull in <alsa/asoundlib.h>.
typedef struct _snd_pcm snd_pcm_t;

namespace nexus::audio {

// Real audio output backed by ALSA (libasound), driving the I2S DAC/amp on the Pi. Built only when
// NEXUS_STUB_HAL is off. Opens the configured PCM device for 16-bit interleaved playback and does
// blocking writes from the playback thread. The device name comes from config (e.g. "hw:0,0" for a
// HiFiBerry-style I2S HAT, or "default").
class AlsaAudioOutputHal : public IAudioOutputHal {
 public:
  explicit AlsaAudioOutputHal(std::string device = "default");
  ~AlsaAudioOutputHal() override;

  core::Status open(int sample_rate, int channels) override;
  core::Status close() override;
  core::Result<std::size_t> write(const std::int16_t* pcm, std::size_t frames) override;

 private:
  std::string device_;
  snd_pcm_t* pcm_ = nullptr;
  int channels_ = 2;
};

}  // namespace nexus::audio
