#pragma once

#include <string>

#include "microphone/IMicrophoneHal.h"

typedef struct _snd_pcm snd_pcm_t;

namespace nexus::microphone {

// Real measurement-microphone capture backed by ALSA (libasound). Built only when NEXUS_STUB_HAL is
// off. Opens the configured capture device (e.g. an I2S MEMS mic HAT) for mono 16-bit capture and
// does blocking reads. Used only for measurement/calibration — never streamed out.
class AlsaMicrophoneHal : public IMicrophoneHal {
 public:
  explicit AlsaMicrophoneHal(std::string device = "default", int sample_rate = 48000);
  ~AlsaMicrophoneHal() override;

  core::Status open() override;
  core::Status close() override;
  core::Result<std::vector<std::int16_t>> capture(std::size_t frames) override;

 private:
  std::string device_;
  int sample_rate_;
  snd_pcm_t* pcm_ = nullptr;
};

}  // namespace nexus::microphone
