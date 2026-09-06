#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "audio/AudioBuffer.h"
#include "audio/IAudioSource.h"
#include "audio/StreamSync.h"
#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::audio {

// Receives the synchronized audio stream from the Streamer: decodes datagrams into AudioPackets,
// feeds the jitter buffer, and updates sync stats. Emits AudioStarted when the stream begins.
// The transport lives behind IAudioSource so the whole receive/buffer/sync path is testable
// off-target.
class AudioReceiver : public core::IService {
 public:
  using Clock = std::function<double()>;  // epoch seconds

  AudioReceiver(core::EventBus* bus, AudioBuffer* buffer, StreamSync* sync, Clock clock,
                std::unique_ptr<IAudioSource> source = nullptr, int port = wire::kDefaultPort,
                int channels = wire::kChannels);

  std::string name() const override { return "audio"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  bool streaming() const { return streaming_.load(); }
  IAudioSource* source() { return source_.get(); }

 private:
  void onDatagram(const std::uint8_t* data, std::size_t len);

  core::EventBus* bus_;
  AudioBuffer* buffer_;
  StreamSync* sync_;
  Clock clock_;
  std::unique_ptr<IAudioSource> source_;
  int port_;
  int channels_;

  std::atomic<bool> streaming_{false};
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::audio
