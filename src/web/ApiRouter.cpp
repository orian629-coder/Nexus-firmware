#include "web/ApiRouter.h"

namespace nexus::web {

HttpResponse ApiRouter::json(int status, const nlohmann::json& body) {
  return {status, "application/json", body.dump()};
}
HttpResponse ApiRouter::ok(const nlohmann::json& body) { return json(200, body); }
HttpResponse ApiRouter::notFound() {
  return json(404, {{"error", "not found"}});
}
HttpResponse ApiRouter::unauthorized() {
  return json(401, {{"error", "unauthorized"}});
}
HttpResponse ApiRouter::actionResult(const std::pair<bool, std::string>& r) {
  return json(r.first ? 200 : 400, {{"ok", r.first}, {"message", r.second}});
}

HttpResponse ApiRouter::route(const HttpRequest& req) const {
  if (req.method == "GET") return handleGet(req.path);
  if (req.method == "POST") return handlePost(req);
  return json(405, {{"error", "method not allowed"}});
}

bool ApiRouter::isSetupMode() const {
  if (!ctx_.status) return false;
  try {
    return ctx_.status().value("setup_mode", false);
  } catch (...) {
    return false;  // if status can't be read, fail closed (require auth)
  }
}

HttpResponse ApiRouter::handleGet(const std::string& path) const {
  if (path == "/api/status" && ctx_.status) return ok(ctx_.status());
  if (path == "/api/health" && ctx_.health) return ok(ctx_.health());
  if (path == "/api/network" && ctx_.network) return ok(ctx_.network());
  if (path == "/api/audio" && ctx_.audio) return ok(ctx_.audio());
  if (path == "/api/hardware" && ctx_.hardware) return ok(ctx_.hardware());
  if (path == "/api/calibration/status" && ctx_.calibrationStatus)
    return ok(ctx_.calibrationStatus());
  if (path == "/api/calibration/result" && ctx_.calibrationResult)
    return ok(ctx_.calibrationResult());
  if (path == "/api/wifi/scan" && ctx_.scanWifi) return ok(ctx_.scanWifi());
  if (path == "/api/system/logs" && ctx_.logs)
    return {200, "text/plain", ctx_.logs()};
  return notFound();
}

HttpResponse ApiRouter::handlePost(const HttpRequest& req) const {
  // Onboarding endpoints must work over the setup hotspot BEFORE the device is online, where the
  // phone has no auth token yet. So the Wi-Fi join and the speaker-name form are open — but ONLY to
  // a visitor who actually came in over the setup AP.
  //
  // Gating on setup_mode alone was not enough: that flag stays true after the device joins the real
  // LAN (it is cleared by pairing, not by connectivity), which left unauthenticated network
  // reconfiguration exposed to anyone on the LAN. Reaching 10.42.0.1 requires being associated with
  // the AP we are hosting, which is the property that makes the exemption safe.
  const bool via_hotspot = req.host.rfind("10.42.0.1", 0) == 0;
  const bool setup_open = isSetupMode() && via_hotspot &&
                          (req.path == "/api/wifi/connect" || req.path == "/api/device/name");
  if (!setup_open && auth_ && !auth_->authorize(req.auth)) return unauthorized();

  nlohmann::json body = nlohmann::json::object();
  if (!req.body.empty()) {
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
      return json(400, {{"error", std::string("bad json: ") + e.what()}});
    }
  }

  const std::string& p = req.path;
  if (p == "/api/audio/volume" && ctx_.setVolume) return actionResult(ctx_.setVolume(body));
  if (p == "/api/audio/mute" && ctx_.setMute) return actionResult(ctx_.setMute(body));
  if (p == "/api/audio/eq" && ctx_.setEq) return actionResult(ctx_.setEq(body));
  if (p == "/api/audio/delay" && ctx_.setDelay) return actionResult(ctx_.setDelay(body));
  if (p == "/api/audio/test" && ctx_.audioTest) return actionResult(ctx_.audioTest());
  if (p == "/api/calibration/start" && ctx_.startCalibration)
    return actionResult(ctx_.startCalibration());
  if (p == "/api/system/reboot" && ctx_.reboot) return actionResult(ctx_.reboot());
  if (p == "/api/system/update" && ctx_.update) return actionResult(ctx_.update());
  if (p == "/api/system/reset-network" && ctx_.resetNetwork)
    return actionResult(ctx_.resetNetwork());
  if (p == "/api/system/factory-reset" && ctx_.factoryReset)
    return actionResult(ctx_.factoryReset());
  if (p == "/api/wifi/connect" && ctx_.connectWifi) return actionResult(ctx_.connectWifi(body));
  if (p == "/api/device/name" && ctx_.setSpeakerName) return actionResult(ctx_.setSpeakerName(body));
  return notFound();
}

}  // namespace nexus::web
