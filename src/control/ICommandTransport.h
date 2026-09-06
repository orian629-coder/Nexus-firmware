#pragma once

#include <functional>
#include <string>

#include "core/Result.h"

namespace nexus::control {

// Transport abstraction for the command server. The transport is responsible only for moving
// bytes: it accepts connections/messages and, for each raw request, calls the handler and writes
// back the response string. All parsing, validation, signing, and execution live above it in
// CommandRouter, so the pipeline is fully testable without a socket.
//
// The real implementation is a POSIX TCP server on port 45455 (Pi). A stub is used off-target and
// in tests, where requests are injected directly via deliver().
class ICommandTransport {
 public:
  // Handler receives a raw request payload and returns the raw response payload.
  using Handler = std::function<std::string(const std::string& request)>;

  virtual ~ICommandTransport() = default;

  virtual core::Status start(int port, Handler handler) = 0;
  virtual core::Status stop() = 0;
};

// In-process transport for dev/tests. start() just stores the handler; tests call deliver() to
// simulate an inbound command and capture the response.
class StubCommandTransport : public ICommandTransport {
 public:
  core::Status start(int, Handler handler) override {
    handler_ = std::move(handler);
    running_ = true;
    return core::Status::success();
  }
  core::Status stop() override {
    running_ = false;
    return core::Status::success();
  }

  // Simulate an inbound request; returns the response the pipeline produced.
  std::string deliver(const std::string& request) {
    if (!running_ || !handler_) return {};
    return handler_(request);
  }

 private:
  Handler handler_;
  bool running_ = false;
};

}  // namespace nexus::control
