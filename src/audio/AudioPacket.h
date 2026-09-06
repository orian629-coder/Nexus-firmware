#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace nexus::audio {

// Wire contract for the synchronized audio stream from the Streamer. Adapted from the prior
// speaker-app SyncProtocol: the Streamer sends UDP packets, each with a 24-byte big-endian header
// carrying an absolute target playback timestamp, followed by interleaved 16-bit PCM samples. All
// speakers release a packet when their (NTP-synced) system clock reaches the timestamp, giving
// sample-aligned multi-room playback. NTP/clock sync is an OS concern (chrony), not implemented
// here.
//
// Header layout (big-endian, 24 bytes, no padding):
//   double   timestamp    — target playback time (seconds since epoch)
//   uint64   sequence     — monotonic sequence number (loss/order detection)
//   uint32   frame_count  — number of PCM frames in the payload
//   uint32   flags        — bit0: last packet of a stream; others reserved
// After the header: frame_count * channels int16 samples (interleaved, little-endian).

namespace wire {
constexpr int kDefaultPort = 50005;
constexpr int kSampleRate = 48000;   // Phase 4 default (matches the ecosystem's 48k/16-bit)
constexpr int kChannels = 2;         // stereo
constexpr std::size_t kHeaderSize = 24;
constexpr std::uint32_t kFlagLast = 0x1;

inline std::uint32_t readBE32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         std::uint32_t(p[3]);
}
inline std::uint64_t readBE64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
inline void writeBE32(std::uint8_t* p, std::uint32_t v) {
  p[0] = std::uint8_t(v >> 24);
  p[1] = std::uint8_t(v >> 16);
  p[2] = std::uint8_t(v >> 8);
  p[3] = std::uint8_t(v);
}
inline void writeBE64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = std::uint8_t(v & 0xFF);
    v >>= 8;
  }
}
}  // namespace wire

struct AudioHeader {
  double timestamp = 0.0;
  std::uint64_t sequence = 0;
  std::uint32_t frame_count = 0;
  std::uint32_t flags = 0;
};

// A decoded packet: header + interleaved 16-bit PCM ready for output.
struct AudioPacket {
  AudioHeader header;
  std::vector<std::int16_t> samples;  // frame_count * channels

  bool isLast() const { return (header.flags & wire::kFlagLast) != 0; }
};

// Decode a raw datagram into an AudioPacket. Returns false if too short or inconsistent.
bool unpackPacket(const std::uint8_t* buf, std::size_t len, int channels, AudioPacket& out);

// Encode an AudioPacket into a datagram (used by tests / a mock streamer).
std::vector<std::uint8_t> packPacket(const AudioPacket& pkt);

}  // namespace nexus::audio
