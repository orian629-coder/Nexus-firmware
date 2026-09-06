#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Result.h"

namespace nexus::web {

struct HttpRequest {
  std::string method;   // "GET", "POST"
  std::string path;     // "/api/status"
  std::string body;     // request body (JSON)
  std::string auth;     // Authorization header value, if any
  std::string host;     // Host header (e.g. "10.42.0.1") — lets the server tell a hotspot visitor
                        // (reached via the setup AP) apart from a LAN dashboard visitor.
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  // Extra response headers (e.g. Location for a 302 redirect). Empty for normal responses.
  std::vector<std::pair<std::string, std::string>> headers;
};

// Transport abstraction for the local web server. It only moves bytes: it listens for HTTP
// requests and hands each to the router, then writes back the response. All routing, auth, and
// business logic live above it (ApiRouter), so the API is fully testable without a socket.
//
// Real impl: cpp-httplib on the Pi. Stub: in-process, tests call handle() directly.
class IWebTransport {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  virtual ~IWebTransport() = default;
  virtual core::Status start(int port, Handler handler) = 0;
  virtual core::Status stop() = 0;
};

// In-process transport for dev/tests. start() stores the handler; tests call handle() to simulate
// a request.
class StubWebTransport : public IWebTransport {
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

  HttpResponse handle(const HttpRequest& req) {
    if (!running_ || !handler_) return {503, "application/json", "{\"error\":\"down\"}", {}};
    return handler_(req);
  }

 private:
  Handler handler_;
  bool running_ = false;
};

}  // namespace nexus::web
