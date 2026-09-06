#include "control/TcpLineTransport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/ErrorCodes.h"

namespace nexus::streamer::control {

using core::ErrorCode;
using core::Result;
using core::Status;

namespace {
void setTimeout(int fd, int ms) {
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
}  // namespace

Result<std::string> TcpLineTransport::request(const std::string& host, int port,
                                              const std::string& request) {
  // Resolve host (accepts dotted IPv4 or a hostname like speaker.local).
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
    return Status::error(ErrorCode::IoError, "resolve failed: " + host);
  }

  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    ::freeaddrinfo(res);
    return Status::error(ErrorCode::IoError, std::string("socket: ") + std::strerror(errno));
  }
  setTimeout(fd, timeout_ms_);

  if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    Status s = Status::error(ErrorCode::IoError, std::string("connect: ") + std::strerror(errno));
    ::freeaddrinfo(res);
    ::close(fd);
    return s;
  }
  ::freeaddrinfo(res);

  // Write request + newline.
  std::string line = request;
  line.push_back('\n');
  std::size_t off = 0;
  while (off < line.size()) {
    const ssize_t n = ::send(fd, line.data() + off, line.size() - off, 0);
    if (n <= 0) {
      ::close(fd);
      return Status::error(ErrorCode::IoError, std::string("send: ") + std::strerror(errno));
    }
    off += static_cast<std::size_t>(n);
  }

  // Read until newline (or EOF).
  std::string resp;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      ::close(fd);
      return Status::error(ErrorCode::IoError, std::string("recv: ") + std::strerror(errno));
    }
    if (n == 0) break;  // peer closed
    resp.append(buf, static_cast<std::size_t>(n));
    if (resp.find('\n') != std::string::npos) break;
  }
  ::close(fd);

  if (auto nl = resp.find('\n'); nl != std::string::npos) resp.erase(nl);
  if (resp.empty()) return Status::error(ErrorCode::IoError, "empty response");
  return resp;
}

}  // namespace nexus::streamer::control
