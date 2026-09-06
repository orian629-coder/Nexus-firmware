#include "control/TcpCommandTransport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "logging/Logger.h"

namespace nexus::control {

using core::ErrorCode;
using core::Status;

TcpCommandTransport::~TcpCommandTransport() { stop(); }

Status TcpCommandTransport::start(int port, Handler handler) {
  handler_ = std::move(handler);
  port_ = port;

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return Status::error(ErrorCode::IoError, std::string("socket: ") + std::strerror(errno));
  }
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Status s = Status::error(ErrorCode::IoError, std::string("bind: ") + std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return s;
  }
  if (::listen(listen_fd_, 8) != 0) {
    Status s = Status::error(ErrorCode::IoError, std::string("listen: ") + std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return s;
  }

  running_ = true;
  thread_ = std::thread([this] { acceptLoop(); });
  NX_LOG_INFO("control", "command server listening on tcp/" + std::to_string(port));
  return Status::success();
}

void TcpCommandTransport::acceptLoop() {
  while (running_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (!running_) break;
      continue;
    }
    // Read one newline-terminated request (bounded).
    std::string req;
    char buf[1024];
    ssize_t n;
    bool done = false;
    while (!done && (n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
      req.append(buf, static_cast<size_t>(n));
      if (req.find('\n') != std::string::npos || req.size() > 64 * 1024) done = true;
    }
    if (auto nl = req.find('\n'); nl != std::string::npos) req.resize(nl);

    std::string resp = handler_ ? handler_(req) : std::string();
    resp += "\n";
    ::send(fd, resp.data(), resp.size(), 0);
    ::close(fd);
  }
}

Status TcpCommandTransport::stop() {
  if (!running_.exchange(false)) return Status::success();
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  return Status::success();
}

}  // namespace nexus::control
