#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::streamer::control {

// Client-side request/response transport for the speaker's newline-JSON control channel: connect to
// host:port, send one request line, return the one response line. The speaker closes the connection
// after each response (request-per-connection), so each call is a fresh dial. Abstracted so the
// CommandClient / PairingClient logic is unit-testable off-target with an in-memory fake, mirroring
// the speaker's ICommandTransport stub/real split. The real implementation (TcpLineTransport) is
// compiled only when NEXUS_STREAMER_REAL_NET is on.
class ILineTransport {
 public:
  virtual ~ILineTransport() = default;

  // Send `request` (a single line WITHOUT the trailing newline; the transport appends it) to
  // host:port and return the response line (trailing newline stripped). Non-ok on connect/IO error.
  virtual core::Result<std::string> request(const std::string& host, int port,
                                            const std::string& request) = 0;
};

}  // namespace nexus::streamer::control
