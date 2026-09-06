#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "web/IWebTransport.h"

namespace httplib {
class Server;
}

namespace nexus::web {

// Real web transport backed by cpp-httplib (Pi). Built only when NEXUS_STUB_HAL is off. Runs the
// server on its own thread; each request is translated to HttpRequest and dispatched to the
// handler, and the HttpResponse is written back.
class HttplibWebTransport : public IWebTransport {
 public:
  HttplibWebTransport();
  ~HttplibWebTransport() override;

  core::Status start(int port, Handler handler) override;
  core::Status stop() override;

 private:
  std::unique_ptr<httplib::Server> server_;
  Handler handler_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace nexus::web
