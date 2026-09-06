#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "send/IPacketSink.h"
#include "send/Packetizer.h"
#include "send/Timestamper.h"
#include "sources/IAudioSource.h"

namespace nexus::streamer::send {

// Phase-1 pipeline driver: pull fixed-size blocks from a source, packetize (future-stamped, shared
// packPacket encoder), and push to a sink. Single-threaded pull is enough for MVP — a source
// produces PCM, we packetize and send it immediately. A true SPSC PcmBus between an async capture
// thread and the sender lands in Phase 4 when live sources (A2DP/AirPlay/Spotify) arrive; the
// IAudioSource seam here is where that ring will slot in without touching the packetizer/sink.
//
// Emits exactly one datagram per full block; a finite source's final (possibly short) block is sent
// with the kFlagLast flag. Returns the number of datagrams sent.
class StreamSender {
 public:
  StreamSender(sources::IAudioSource& source, Timestamper& ts, Packetizer& pk, IPacketSink& sink,
               int channels, std::uint32_t block_frames)
      : source_(source), ts_(ts), pk_(pk), sink_(sink), channels_(channels),
        block_frames_(block_frames) {}

  // Drain a finite source to end-of-stream. `now_epoch` anchors the stream start (START_AUDIO).
  std::size_t runToEnd(double now_epoch) {
    if (!ts_.started()) ts_.start(now_epoch);
    std::size_t sent = 0;
    std::vector<std::int16_t> buf;
    for (;;) {
      buf.clear();
      const std::size_t frames = source_.read(block_frames_, buf);
      if (frames == 0) break;
      const bool last = source_.exhausted();
      sink_.send(pk_.pack(buf.data(), static_cast<std::uint32_t>(frames), last));
      ++sent;
      if (last) break;
    }
    return sent;
  }

 private:
  sources::IAudioSource& source_;
  Timestamper& ts_;
  Packetizer& pk_;
  IPacketSink& sink_;
  int channels_;
  std::uint32_t block_frames_;
};

}  // namespace nexus::streamer::send
