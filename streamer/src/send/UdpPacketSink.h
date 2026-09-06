#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/Result.h"
#include "send/IPacketSink.h"

namespace nexus::streamer::send {

// Real sink: sends each datagram to a set of speaker endpoints via a single UDP socket. MVP uses one
// target; addTarget/removeTarget make Phase-3 multi-room fan-out a set operation over the same
// encoded bytes (identical timestamped datagrams → sample-aligned playback). Built only when
// NEXUS_STUB_HAL is off (needs real sockets), matching the speaker's real/stub HAL split.
class UdpPacketSink : public IPacketSink {
 public:
  UdpPacketSink() = default;
  ~UdpPacketSink() override;

  core::Status open();
  core::Status addTarget(const std::string& host, int port);   // host = IPv4 dotted or resolvable
  void removeTarget(const std::string& host, int port);
  std::size_t targetCount() const;

  // Atomically replace the fan-out set (see IPacketSink::setTargets). Safe to call from another
  // thread while the audio thread is sending.
  core::Status setTargets(const std::vector<std::pair<std::string, int>>& targets) override;

  bool send(const std::vector<std::uint8_t>& datagram) override;

 private:
  struct Target {
    std::string host;
    int port;
    std::uint32_t addr_be;  // resolved IPv4, network byte order
  };

  // Accepts a dotted IPv4 or a resolvable hostname. Speakers are registered by whatever the UI or
  // mDNS supplied — a ".local" name or a hostname — while the audio path needs a numeric address,
  // so resolution happens here rather than rejecting the target.
  static core::Result<std::uint32_t> resolveIpv4(const std::string& host);

  int fd_ = -1;
  mutable std::mutex targets_mutex_;  // the audio thread sends while the web thread retargets
  std::vector<Target> targets_;
};

}  // namespace nexus::streamer::send
