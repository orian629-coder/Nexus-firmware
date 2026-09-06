#include "send/UdpPacketSink.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace nexus::streamer::send {

using core::ErrorCode;
using core::Status;

UdpPacketSink::~UdpPacketSink() {
  if (fd_ >= 0) ::close(fd_);
}

Status UdpPacketSink::open() {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    return Status::error(ErrorCode::IoError, std::string("socket: ") + std::strerror(errno));
  }
  int sndbuf = 4 * 1024 * 1024;
  ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  return Status::success();
}

core::Result<std::uint32_t> UdpPacketSink::resolveIpv4(const std::string& host) {
  in_addr a{};
  if (::inet_pton(AF_INET, host.c_str(), &a) == 1) return static_cast<std::uint32_t>(a.s_addr);

  addrinfo hints{};
  hints.ai_family = AF_INET;  // the wire format is IPv4-only
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
    return Status::error(ErrorCode::InvalidArg, "cannot resolve target host: " + host);
  }
  const auto addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
  ::freeaddrinfo(res);
  return static_cast<std::uint32_t>(addr);
}

Status UdpPacketSink::addTarget(const std::string& host, int port) {
  auto addr = resolveIpv4(host);
  if (!addr.ok()) return addr.status();
  std::lock_guard<std::mutex> lk(targets_mutex_);
  targets_.push_back({host, port, addr.value()});
  return Status::success();
}

void UdpPacketSink::removeTarget(const std::string& host, int port) {
  std::lock_guard<std::mutex> lk(targets_mutex_);
  for (auto it = targets_.begin(); it != targets_.end(); ++it) {
    if (it->host == host && it->port == port) {
      targets_.erase(it);
      return;
    }
  }
}

std::size_t UdpPacketSink::targetCount() const {
  std::lock_guard<std::mutex> lk(targets_mutex_);
  return targets_.size();
}

Status UdpPacketSink::setTargets(const std::vector<std::pair<std::string, int>>& targets) {
  // Resolve everything before taking the lock so a slow DNS lookup never stalls the audio thread,
  // and so a bad host leaves the current set untouched rather than half-applied.
  std::vector<Target> next;
  next.reserve(targets.size());
  for (const auto& [host, port] : targets) {
    auto addr = resolveIpv4(host);
    if (!addr.ok()) return addr.status();
    next.push_back({host, port, addr.value()});
  }
  std::lock_guard<std::mutex> lk(targets_mutex_);
  targets_ = std::move(next);
  return Status::success();
}

bool UdpPacketSink::send(const std::vector<std::uint8_t>& datagram) {
  if (fd_ < 0) return false;
  std::vector<Target> targets;
  {
    std::lock_guard<std::mutex> lk(targets_mutex_);
    targets = targets_;  // copy so the sendto() loop never holds the lock
  }
  bool all_ok = true;
  for (const auto& t : targets) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = t.addr_be;
    addr.sin_port = htons(static_cast<uint16_t>(t.port));
    const ssize_t n = ::sendto(fd_, datagram.data(), datagram.size(), 0,
                               reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (n != static_cast<ssize_t>(datagram.size())) all_ok = false;  // best-effort UDP
  }
  return all_ok;
}

}  // namespace nexus::streamer::send
