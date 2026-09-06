#pragma once

#include <string>

#include "control/ILineTransport.h"
#include "core/Result.h"

namespace nexus::streamer::control {

// Real client transport: for each request opens a TCP connection to host:port, writes the request
// line + '\n', reads until '\n', closes. Matches the speaker's request-per-connection server
// (TcpCommandTransport). Blocking with a bounded connect/read timeout. Compiled only when
// NEXUS_STREAMER_REAL_NET is on.
class TcpLineTransport : public ILineTransport {
 public:
  explicit TcpLineTransport(int timeout_ms = 3000) : timeout_ms_(timeout_ms) {}

  core::Result<std::string> request(const std::string& host, int port,
                                    const std::string& request) override;

 private:
  int timeout_ms_;
};

}  // namespace nexus::streamer::control
