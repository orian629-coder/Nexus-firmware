#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "audio/AudioBuffer.h"
#include "audio/IAudioOutputHal.h"
#include "audio/PlaybackManager.h"
#include "audio/StreamSync.h"
#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::audio {

// Drives audio playback: an IService that owns the output device and runs a real-time thread which
// pulls decoded packets from the jitter buffer, runs the DSP chain, and writes PCM to the output
// HAL. This closes the loop between AudioReceiver (which fills the buffer) and the speaker.
//
// The output HAL (StubAudioOutputHal off-target, AlsaAudioOutputHal on the Pi) and the DSP hook are
// injected so the engine is testable and decoupled. The thread paces itself by how fast the output
// consumes; when the buffer underflows it idles briefly and re-prefills.
class PlaybackEngine : public core::IService {
 public:
  using Clock = std::function<double()>;  // epoch seconds
  using DspHook = PlaybackManager::DspHook;

  PlaybackEngine(core::EventBus* bus, AudioBuffer* buffer, StreamSync* sync,
                 config::ConfigManager* config, Clock clock,
                 std::unique_ptr<IAudioOutputHal> output = nullptr, int sample_rate = 48000,
                 int channels = 2);

  std::string name() const override { return "playback"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  void setDspHook(DspHook hook);

  std::uint64_t framesPlayed() const;

 private:
  void loop();

  core::EventBus* bus_;
  AudioBuffer* buffer_;
  StreamSync* sync_;
  config::ConfigManager* config_;
  Clock clock_;
  std::unique_ptr<IAudioOutputHal> output_;
  int sample_rate_;
  int channels_;
  std::unique_ptr<PlaybackManager> pb_;
  DspHook dsp_hook_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::audio
