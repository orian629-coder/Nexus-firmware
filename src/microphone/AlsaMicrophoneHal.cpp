#include "microphone/AlsaMicrophoneHal.h"

#include <alsa/asoundlib.h>

#include "logging/Logger.h"

namespace nexus::microphone {

using core::ErrorCode;
using core::Result;
using core::Status;

AlsaMicrophoneHal::AlsaMicrophoneHal(std::string device, int sample_rate)
    : device_(std::move(device)), sample_rate_(sample_rate) {}

AlsaMicrophoneHal::~AlsaMicrophoneHal() { close(); }

Status AlsaMicrophoneHal::open() {
  int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    pcm_ = nullptr;
    return Status::error(ErrorCode::IoError, std::string("snd_pcm_open: ") + snd_strerror(err));
  }
  err = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           /*channels=*/1, static_cast<unsigned int>(sample_rate_),
                           /*soft_resample=*/1, /*latency_us=*/100000);
  if (err < 0) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return Status::error(ErrorCode::IoError,
                         std::string("snd_pcm_set_params: ") + snd_strerror(err));
  }
  NX_LOG_INFO("microphone", "alsa mic open on " + device_);
  return Status::success();
}

Status AlsaMicrophoneHal::close() {
  if (pcm_) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
  return Status::success();
}

Result<std::vector<std::int16_t>> AlsaMicrophoneHal::capture(std::size_t frames) {
  if (!pcm_) return Status::error(ErrorCode::IoError, "mic not open");
  std::vector<std::int16_t> pcm(frames);  // mono
  std::size_t got = 0;
  while (got < frames) {
    snd_pcm_sframes_t r = snd_pcm_readi(pcm_, pcm.data() + got, frames - got);
    if (r < 0) {
      r = snd_pcm_recover(pcm_, static_cast<int>(r), /*silent=*/1);
      if (r < 0) {
        return Status::error(ErrorCode::IoError,
                             std::string("snd_pcm_readi: ") + snd_strerror(static_cast<int>(r)));
      }
      continue;
    }
    got += static_cast<std::size_t>(r);
  }
  return pcm;
}

}  // namespace nexus::microphone
