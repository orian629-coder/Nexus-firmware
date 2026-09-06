#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "send/IPacketSink.h"

namespace nexus::streamer::send {

// In-memory sink for tests: records every datagram so the pipeline output can be decoded with the
// speaker's unpackPacket and asserted (sequence order, future timestamps, exact PCM round-trip).
//
// Guarded because AudioEngine sends from its own thread while the test thread reads the recording.
class MemoryPacketSink : public IPacketSink {
 public:
  bool send(const std::vector<std::uint8_t>& datagram) override {
    std::lock_guard<std::mutex> lk(mutex_);
    datagrams_.push_back(datagram);
    return true;
  }

  std::vector<std::vector<std::uint8_t>> datagrams() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return datagrams_;
  }

  std::size_t count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return datagrams_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::vector<std::uint8_t>> datagrams_;
};

}  // namespace nexus::streamer::send
