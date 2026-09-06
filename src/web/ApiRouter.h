#pragma once

#include <string>

#include "web/ApiContext.h"
#include "web/Authentication.h"
#include "web/IWebTransport.h"

namespace nexus::web {

// Pure HTTP request router for the local technician API. Maps (method, path, body) to a JSON
// response using the injected ApiContext callbacks. Read endpoints are open on the LAN; state-
// changing endpoints require authentication. No socket here — fully unit-testable.
//
// Endpoints (per spec):
//   GET  /api/status /health /network /audio /hardware
//   POST /api/audio/volume /mute /eq /delay /test
//   POST /api/calibration/start   GET /api/calibration/status /result
//   POST /api/system/reboot /update /reset-network /factory-reset   GET /api/system/logs
class ApiRouter {
 public:
  ApiRouter(ApiContext ctx, Authentication* auth) : ctx_(std::move(ctx)), auth_(auth) {}

  HttpResponse route(const HttpRequest& req) const;

 private:
  HttpResponse handleGet(const std::string& path) const;
  HttpResponse handlePost(const HttpRequest& req) const;
  // True while the device is unprovisioned (status reports setup_mode). Used to open the onboarding
  // POST endpoints before an auth token exists.
  bool isSetupMode() const;

  static HttpResponse json(int status, const nlohmann::json& body);
  static HttpResponse ok(const nlohmann::json& body);
  static HttpResponse actionResult(const std::pair<bool, std::string>& r);
  static HttpResponse notFound();
  static HttpResponse unauthorized();

  ApiContext ctx_;
  Authentication* auth_;
};

}  // namespace nexus::web
