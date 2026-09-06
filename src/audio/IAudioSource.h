#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/Result.h"

namespace nexus::audio {

// Source of raw audio datagrams from the Streamer. The real implementation is a UDP socket on the
// sync port (Pi); a stub lets tests inject packets directly, so the receiver/buffer/sync logic is
// fully exercised off-target. Selected by NEXUS_STUB_HAL.
class IAudioSource {
 public:
  using PacketHandler = std::function<void(const std::uint8_t* data, std::size_t len)>;

  virtual ~IAudioSource() = default;

  // Begin receiving on `port`; each datagram is delivered to `handler` (on the source's own
  // thread for the real UDP impl).
  virtual core::Status start(int port, PacketHandler handler) = 0;
  virtual core::Status stop() = 0;
};

// In-process source for dev/tests. start() records the handler; tests call inject() to simulate an
// inbound datagram.
class StubAudioSource : public IAudioSource {
 public:
  core::Status start(int, PacketHandler handler) override {
    handler_ = std::move(handler);
    running_ = true;
    return core::Status::success();
  }
  core::Status stop() override {
    running_ = false;
    return core::Status::success();
  }

  void inject(const std::vector<std::uint8_t>& datagram) {
    if (running_ && handler_) handler_(datagram.data(), datagram.size());
  }

 private:
  PacketHandler handler_;
  bool running_ = false;
};

}  // namespace nexus::audio
