#pragma once

#include <atomic>
#include <thread>

#include "control/ICommandTransport.h"

namespace nexus::control {

// POSIX TCP command transport. Listens on the given port (45455) and, for each connection, reads a
// single newline-terminated JSON request, invokes the handler, and writes the newline-terminated
// response. One accept thread; each connection handled inline (commands are infrequent and small).
// Built only when NEXUS_STUB_HAL is off.
class TcpCommandTransport : public ICommandTransport {
 public:
  ~TcpCommandTransport() override;

  core::Status start(int port, Handler handler) override;
  core::Status stop() override;

 private:
  void acceptLoop();

  int listen_fd_ = -1;
  int port_ = 0;
  Handler handler_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace nexus::control
