#include "audio/AudioPacket.h"

namespace nexus::audio {

bool unpackPacket(const std::uint8_t* buf, std::size_t len, int channels, AudioPacket& out) {
  if (len < wire::kHeaderSize) return false;

  std::uint64_t ts_bits = wire::readBE64(buf);
  std::memcpy(&out.header.timestamp, &ts_bits, sizeof(double));
  out.header.sequence = wire::readBE64(buf + 8);
  out.header.frame_count = wire::readBE32(buf + 16);
  out.header.flags = wire::readBE32(buf + 20);

  const std::size_t sample_count = static_cast<std::size_t>(out.header.frame_count) *
                                   static_cast<std::size_t>(channels);
  const std::size_t need = wire::kHeaderSize + sample_count * sizeof(std::int16_t);
  if (len < need) return false;

  out.samples.resize(sample_count);
  // Payload is little-endian int16; copy directly (target platforms are little-endian).
  std::memcpy(out.samples.data(), buf + wire::kHeaderSize, sample_count * sizeof(std::int16_t));
  return true;
}

std::vector<std::uint8_t> packPacket(const AudioPacket& pkt) {
  const std::size_t sample_bytes = pkt.samples.size() * sizeof(std::int16_t);
  std::vector<std::uint8_t> buf(wire::kHeaderSize + sample_bytes);

  std::uint64_t ts_bits;
  std::memcpy(&ts_bits, &pkt.header.timestamp, sizeof(double));
  wire::writeBE64(buf.data(), ts_bits);
  wire::writeBE64(buf.data() + 8, pkt.header.sequence);
  wire::writeBE32(buf.data() + 16, pkt.header.frame_count);
  wire::writeBE32(buf.data() + 20, pkt.header.flags);
  std::memcpy(buf.data() + wire::kHeaderSize, pkt.samples.data(), sample_bytes);
  return buf;
}

}  // namespace nexus::audio
