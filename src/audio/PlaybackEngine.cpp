#include "audio/PlaybackEngine.h"

#include <chrono>
#include <vector>

#include "logging/Logger.h"

#if !NEXUS_STUB_HAL
#include "audio/AlsaAudioOutputHal.h"
#endif

namespace nexus::audio {

using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IAudioOutputHal> makeDefaultOutput(const std::string& device) {
#if NEXUS_STUB_HAL
  (void)device;
  return std::make_unique<StubAudioOutputHal>();
#else
  return std::make_unique<AlsaAudioOutputHal>(device.empty() ? "default" : device);
#endif
}
}  // namespace

PlaybackEngine::PlaybackEngine(core::EventBus* bus, AudioBuffer* buffer, StreamSync* sync,
                               config::ConfigManager* config, Clock clock,
                               std::unique_ptr<IAudioOutputHal> output, int sample_rate,
                               int channels)
    : bus_(bus),
      buffer_(buffer),
      sync_(sync),
      config_(config),
      clock_(std::move(clock)),
      output_(std::move(output)),  // may be null; built lazily in start() from config
      sample_rate_(sample_rate),
      channels_(channels) {}

void PlaybackEngine::setDspHook(DspHook hook) {
  dsp_hook_ = std::move(hook);
  if (pb_) pb_->setDspHook(dsp_hook_);
}

Status PlaybackEngine::start() {
  // Build the output HAL now — config is loaded by this point, so the ALSA device name is correct
  // (constructing it earlier would capture the default before config was read).
  if (!output_) {
    output_ = makeDefaultOutput(config_ ? config_->get().audio.output_device : "default");
  }
  if (!pb_) {
    pb_ = std::make_unique<PlaybackManager>(buffer_, sync_, output_.get(), config_, clock_);
    if (dsp_hook_) pb_->setDspHook(dsp_hook_);
  }
  Status s = pb_->open(sample_rate_, channels_);
  if (!s.ok()) {
    state_ = ServiceState::Degraded;
    NX_LOG_ERROR("playback", s.code(), "audio output open failed: " + s.message());
    return s;
  }
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  state_ = ServiceState::Running;
  return Status::success();
}

void PlaybackEngine::loop() {
  // The output HAL's blocking write paces the loop to real time — but ONLY once the ALSA ring
  // buffer is full. Until then writei returns immediately, so a loop that sleeps whenever the jitter
  // buffer is momentarily empty starves the device instead: ALSA drains its 80 ms buffer in real
  // time while the network only delivers one 10 ms packet per 10 ms, the loop cannot get ahead, and
  // the stream dies with an XRUN before a single sample is audible. Observed on the Pi as
  // /proc/asound/.../status reporting "state: XRUN" while every log line said PLAYING.
  //
  // The fix is to keep the device fed: on a gap, write one block of silence rather than sleeping.
  // Silence costs nothing audible, keeps the PCM running so it never underruns, and — because that
  // write blocks once the buffer is full — it also paces this loop correctly while idle.
  const std::size_t silence_frames = static_cast<std::size_t>(sample_rate_) / 100;  // 10 ms
  std::vector<std::int16_t> silence(silence_frames * static_cast<std::size_t>(channels_), 0);

  while (running_) {
    const std::size_t frames = pb_->step();
    if (frames > 0) continue;

    if (output_) {
      output_->write(silence.data(), silence_frames);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
}

Status PlaybackEngine::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (pb_) pb_->close();
  state_ = ServiceState::Stopped;
  return Status::success();
}

std::uint64_t PlaybackEngine::framesPlayed() const { return pb_ ? pb_->framesPlayed() : 0; }

}  // namespace nexus::audio
