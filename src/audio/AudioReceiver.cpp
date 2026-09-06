#include "audio/AudioReceiver.h"

#include "audio/AudioPacket.h"
#include "logging/Logger.h"

#if NEXUS_STUB_HAL
// StubAudioSource is defined in IAudioSource.h.
#else
#include "audio/UdpAudioSource.h"
#endif

namespace nexus::audio {

using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IAudioSource> makeDefaultSource() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubAudioSource>();
#else
  return std::make_unique<UdpAudioSource>();
#endif
}
}  // namespace

AudioReceiver::AudioReceiver(core::EventBus* bus, AudioBuffer* buffer, StreamSync* sync,
                             Clock clock, std::unique_ptr<IAudioSource> source, int port,
                             int channels)
    : bus_(bus),
      buffer_(buffer),
      sync_(sync),
      clock_(std::move(clock)),
      source_(source ? std::move(source) : makeDefaultSource()),
      port_(port),
      channels_(channels) {}

Status AudioReceiver::start() {
  Status s = source_->start(port_, [this](const std::uint8_t* d, std::size_t n) {
    onDatagram(d, n);
  });
  state_ = s.ok() ? ServiceState::Running : ServiceState::Degraded;
  return s;
}

Status AudioReceiver::stop() {
  if (source_) source_->stop();
  if (streaming_.exchange(false) && bus_) {
    bus_->publish(core::Event{core::EventType::AudioStopped, "audio"});
  }
  state_ = ServiceState::Stopped;
  return Status::success();
}

void AudioReceiver::onDatagram(const std::uint8_t* data, std::size_t len) {
  AudioPacket pkt;
  if (!unpackPacket(data, len, channels_, pkt)) {
    NX_LOG_WARN("audio", "dropped malformed audio datagram");
    return;
  }

  const double now = clock_();
  sync_->observe(pkt.header.timestamp, now);
  buffer_->push(pkt);

  if (!streaming_.exchange(true) && bus_) {
    NX_LOG_INFO("audio", "audio stream started");
    bus_->publish(core::Event{core::EventType::AudioStarted, "audio"});
  }

  if (pkt.isLast() && streaming_.exchange(false) && bus_) {
    NX_LOG_INFO("audio", "audio stream ended (last packet)");
    bus_->publish(core::Event{core::EventType::AudioStopped, "audio"});
  }
}

}  // namespace nexus::audio
