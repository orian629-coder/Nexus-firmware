#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/Result.h"

namespace nexus::streamer::send {

// Destination for encoded audio datagrams. The real sink is a UDP socket fanning out to N speakers;
// the test/stub sink records datagrams in memory so the whole packetize→send pipeline runs and is
// asserted off-target (mirrors the speaker's IAudioSource/StubAudioSource split, selected by
// NEXUS_STUB_HAL). For MVP there is one target; the interface already takes a per-datagram call so
// multi-room fan-out in Phase 3 is just "send to each target".
class IPacketSink {
 public:
  virtual ~IPacketSink() = default;

  // Send one encoded datagram to all current targets. Best-effort (UDP); returns false only on a
  // hard local error (e.g. socket closed), not on a dropped packet.
  virtual bool send(const std::vector<std::uint8_t>& datagram) = 0;

  // Replace the current fan-out set. Every target receives byte-identical datagrams, which is the
  // whole basis of sample-aligned multi-room. Declared here (rather than only on UdpPacketSink) so
  // AudioEngine can retarget a stream without knowing whether it is driving a UDP socket or a test
  // recorder. Default: no targets to manage, which is correct for an in-memory sink.
  virtual core::Status setTargets(const std::vector<std::pair<std::string, int>>& /*targets*/) {
    return core::Status::success();
  }
};

}  // namespace nexus::streamer::send
