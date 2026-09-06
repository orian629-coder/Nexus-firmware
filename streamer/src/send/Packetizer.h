#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/AudioPacket.h"
#include "send/Timestamper.h"

namespace nexus::streamer::send {

// Turns interleaved int16 stereo PCM into wire packets. It builds a nexus::audio::AudioPacket (the
// speaker's own struct) and encodes it with nexus::audio::packPacket — so the byte layout is SHARED
// with the speaker, never re-implemented. Each block gets a monotonic sequence number and a future
// timestamp from the Timestamper. The last block of a finite stream sets kFlagLast.
class Packetizer {
 public:
  Packetizer(Timestamper& ts, int channels) : ts_(ts), channels_(channels) {}

  // Build one wire datagram from exactly `frame_count` frames starting at `frames` (which must hold
  // frame_count*channels interleaved int16). `last` sets the end-of-stream flag. Returns the encoded
  // bytes ready for sendto().
  std::vector<std::uint8_t> pack(const std::int16_t* frames, std::uint32_t frame_count, bool last) {
    nexus::audio::AudioPacket pkt;
    pkt.header.timestamp = ts_.nextTimestamp();
    pkt.header.sequence = sequence_++;
    pkt.header.frame_count = frame_count;
    pkt.header.flags = last ? nexus::audio::wire::kFlagLast : 0u;
    const std::size_t n = static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(channels_);
    pkt.samples.assign(frames, frames + n);
    return nexus::audio::packPacket(pkt);
  }

  std::uint64_t nextSequence() const { return sequence_; }

 private:
  Timestamper& ts_;
  int channels_;
  std::uint64_t sequence_ = 0;
};

}  // namespace nexus::streamer::send
