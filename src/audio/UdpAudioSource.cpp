#include "audio/UdpAudioSource.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "logging/Logger.h"

namespace nexus::audio {

using core::ErrorCode;
using core::Status;

UdpAudioSource::~UdpAudioSource() { stop(); }

Status UdpAudioSource::start(int port, PacketHandler handler) {
  handler_ = std::move(handler);
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    return Status::error(ErrorCode::IoError, std::string("socket: ") + std::strerror(errno));
  }
  int one = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int rcvbuf = 8 * 1024 * 1024;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Status s = Status::error(ErrorCode::IoError, std::string("bind: ") + std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return s;
  }

  running_ = true;
  thread_ = std::thread([this] { recvLoop(); });
  NX_LOG_INFO("audio", "udp audio source listening on udp/" + std::to_string(port));
  return Status::success();
}

void UdpAudioSource::recvLoop() {
  std::vector<std::uint8_t> buf(64 * 1024);
  while (running_) {
    ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
    if (n <= 0) {
      if (!running_) break;
      continue;
    }
    if (handler_) handler_(buf.data(), static_cast<std::size_t>(n));
  }
}

Status UdpAudioSource::stop() {
  if (!running_.exchange(false)) return Status::success();
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  return Status::success();
}

}  // namespace nexus::audio
