#pragma once

#include <memory>
#include <string>

#include "core/EventBus.h"
#include "core/IService.h"
#include "web/ApiContext.h"
#include "web/ApiRouter.h"
#include "web/Authentication.h"
#include "web/IWebTransport.h"

namespace nexus::web {

// Local technician web interface + REST API. Serves the embedded dashboard at "/" and the API
// under "/api/...". The HTTP transport is behind IWebTransport (StubWebTransport for tests,
// HttplibWebTransport on the Pi) so all routing/auth logic is testable without a socket.
class WebServer : public core::IService {
 public:
  static constexpr int kDefaultPort = 8080;

  WebServer(core::EventBus* bus, ApiContext ctx, std::string auth_token,
            std::unique_ptr<IWebTransport> transport = nullptr, int port = kDefaultPort);

  std::string name() const override { return "web"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Update the maintenance token (e.g. once the device identity is loaded at startup).
  void setAuthToken(const std::string& token) { auth_.setToken(token); }

  // Handle one request (frontend at "/", API under "/api"). Exposed for tests and reused by the
  // transport.
  HttpResponse handle(const HttpRequest& req) const;

  IWebTransport* transport() { return transport_.get(); }

 private:
  // True when the speaker isn't yet on a real network (setup mode). In that state the root "/" and
  // captive-portal probes serve the Wi-Fi setup page instead of the read-only dashboard, so a phone
  // joining the hotspot lands directly on onboarding.
  bool inSetupMode() const;

  core::EventBus* bus_;
  int port_;
  Authentication auth_;
  std::function<nlohmann::json()> status_;  // ctx.status, for setup-mode detection (init before router_)
  ApiRouter router_;
  std::unique_ptr<IWebTransport> transport_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::web
