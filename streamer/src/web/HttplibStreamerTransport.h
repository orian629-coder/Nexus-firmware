#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "web/IWebTransport.h"

namespace httplib {
class Server;
}

namespace nexus::streamer::web {

// Real HTTP transport for the streamer's control UI, backed by cpp-httplib. Reuses the speaker's
// nexus::web::IWebTransport contract (HttpRequest/HttpResponse) so the router is shared and testable
// with StubWebTransport. Compiled only when NEXUS_STREAMER_REAL_NET is on.
class HttplibStreamerTransport : public nexus::web::IWebTransport {
 public:
  HttplibStreamerTransport();
  ~HttplibStreamerTransport() override;

  // Interface to listen on. Defaults to loopback so the control API — which can pair a speaker and
  // therefore accepts a Wi-Fi PSK — is not exposed to the LAN unless asked for (--bind 0.0.0.0).
  // Must be called before start().
  void setBindAddress(std::string address) { bind_address_ = std::move(address); }

  core::Status start(int port, Handler handler) override;
  core::Status stop() override;

 private:
  std::string bind_address_ = "127.0.0.1";
  std::unique_ptr<httplib::Server> server_;
  Handler handler_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace nexus::streamer::web
