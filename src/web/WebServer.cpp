#include "web/WebServer.h"

#include <cstdlib>
#include <string>

#include "logging/Logger.h"
#include "web/Frontend.h"
#include "web/QrEncoder.h"

#if NEXUS_STUB_HAL
// StubWebTransport is defined in IWebTransport.h.
#else
#include "web/HttplibWebTransport.h"
#endif

namespace nexus::web {

using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IWebTransport> makeDefaultTransport() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubWebTransport>();
#else
  return std::make_unique<HttplibWebTransport>();
#endif
}
}  // namespace

WebServer::WebServer(core::EventBus* bus, ApiContext ctx, std::string auth_token,
                     std::unique_ptr<IWebTransport> transport, int port)
    : bus_(bus),
      port_(port),
      auth_(std::move(auth_token)),
      status_(ctx.status),  // copy the status callback before ctx is moved into the router
      router_(std::move(ctx), &auth_),
      transport_(transport ? std::move(transport) : makeDefaultTransport()) {}

bool WebServer::inSetupMode() const {
  if (!status_) return false;
  try {
    auto s = status_();
    return s.value("setup_mode", false);
  } catch (...) {
    return false;
  }
}

HttpResponse WebServer::handle(const HttpRequest& req) const {
  // In setup mode (not yet on a real network) the root serves the Wi-Fi onboarding page, so a phone
  // that joins the hotspot and opens http://10.42.0.1 lands directly on setup. Once online, "/"
  // serves the read-only dashboard as usual.
  // No-store on the onboarding pages: captive-portal browsers aggressively cache, which showed a
  // stale setup page after updates. Force a fresh fetch every time.
  const std::vector<std::pair<std::string, std::string>> kNoCache = {
      {"Cache-Control", "no-store, no-cache, must-revalidate"}, {"Pragma", "no-cache"}};

  const bool setup = inSetupMode();
  // "Reached via the setup hotspot" = the Host header is the AP's fixed address. A visitor who
  // joined "Nexus-Setup" and opened http://10.42.0.1 (with or without /setup) always wants the
  // interactive onboarding page — even if the speaker is already paired (they came to reconfigure
  // Wi-Fi). Only a real-LAN visitor at the device's own IP gets the read-only dashboard.
  const bool via_hotspot = req.host.rfind("10.42.0.1", 0) == 0;
  if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
    const bool want_setup = setup || via_hotspot;
    return {200, "text/html",
            want_setup ? Frontend::setupHtml(auth_.token()) : Frontend::indexHtml(), kNoCache};
  }
  // Interactive setup UI: pick a Wi-Fi network and join it (used during onboarding, reachable over
  // the setup hotspot / captive portal). Distinct from the read-only dashboard at "/".
  if (req.method == "GET" && req.path == "/setup") {
    return {200, "text/html", Frontend::setupHtml(auth_.token()), kNoCache};
  }
  // Setup-hotspot QR as SVG: scanning it joins the "Nexus-Setup" AP so the captive portal opens
  // /setup. SSID/password come from the same env the hotspot service uses (scripts/hotspot.sh), so
  // the QR always matches the AP actually raised. No external assets — encoded server-side.
  if (req.method == "GET" && req.path == "/api/setup-qr") {
    auto env = [](const char* k, const char* dflt) {
      const char* v = std::getenv(k);
      return std::string((v && *v) ? v : dflt);
    };
    const std::string ssid = env("NEXUS_HOTSPOT_SSID", "Nexus-Setup");
    // Empty by default: the setup AP is open (scripts/hotspot.sh).
    const std::string pass = env("NEXUS_HOTSPOT_PASSWORD", "");
    auto esc = [](const std::string& v) {
      std::string o;
      for (char c : v) {
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
        o += c;
      }
      return o;
    };
    // An open network is advertised as T:nopass with no P: field; sending T:WPA for an open AP
    // makes the phone try (and fail) to join with a password.
    const std::string payload =
        pass.empty() ? ("WIFI:T:nopass;S:" + esc(ssid) + ";;")
                     : ("WIFI:T:WPA;S:" + esc(ssid) + ";P:" + esc(pass) + ";;");
    try {
      return {200, "image/svg+xml", QrEncoder::toSvg(payload)};
    } catch (const std::exception& e) {
      return {500, "application/json", std::string("{\"error\":\"") + e.what() + "\"}"};
    }
  }
  // URL QR: encodes the AP address as a plain http:// URL so scanning it OPENS THE BROWSER straight
  // on the onboarding page — no reliance on the captive-portal auto-pop (which proved unreliable).
  // We encode the bare "http://10.42.0.1" (not /setup): a visit reached via the hotspot Host always
  // serves the interactive setup page (see the root handler above), so the short URL is enough — and
  // a shorter URL means a less dense, easier-to-scan QR. The phone must already be on "Nexus-Setup"
  // for 10.42.0.1 to resolve; the flow is: join the AP, then scan this to jump to the page.
  if (req.method == "GET" && req.path == "/api/setup-url-qr") {
    try {
      return {200, "image/svg+xml", QrEncoder::toSvg("http://10.42.0.1")};
    } catch (const std::exception& e) {
      return {500, "application/json", std::string("{\"error\":\"") + e.what() + "\"}"};
    }
  }
  if (req.path.rfind("/api", 0) == 0) return router_.route(req);

  // ── Captive-portal detection ──
  // After joining a Wi-Fi network, phones fetch a known probe URL to decide "is there internet, or a
  // captive portal?". To make the setup page auto-pop reliably on BOTH platforms, we must answer
  // each probe the way that OS expects to see "you're behind a portal":
  //   • Android/ChromeOS: GET /generate_204 (and /gen_204). Expects HTTP 204 with empty body when
  //     online. Returning a 302 to our page signals "portal" and pops it.
  //   • iOS/macOS: GET /hotspot-detect.html (Host: captive.apple.com). Expects a body containing
  //     exactly "Success". Returning our redirect/HTML (not "Success") pops the captive sheet.
  //   • Windows: GET /connecttest.txt or /ncsi.txt → same treatment.
  // Only do this while actually in setup mode (hosting the AP). On the real LAN we're behind the
  // router and these paths are unreachable anyway, but gating on setup avoids surprising a probe that
  // reaches us on a normal network.
  if (req.method == "GET" && setup) {
    // A blanket 302 to the setup page is what triggers the portal window on Android and iOS alike.
    // (iOS treats any non-"Success" response to its probe as a captive portal.)
    return {302, "text/html", "", {{"Location", "http://10.42.0.1/setup"}, kNoCache[0], kNoCache[1]}};
  }
  // Not in setup mode: a probe reaching us means we're online — answer "success" so the OS doesn't
  // falsely show a portal. Android gets its 204; everyone else gets Apple's "Success" body.
  if (req.method == "GET" && (req.path == "/generate_204" || req.path == "/gen_204")) {
    return {204, "text/plain", ""};
  }
  if (req.method == "GET") {
    return {200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"};
  }
  return {404, "application/json", "{\"error\":\"not found\"}"};
}

Status WebServer::start() {
  Status s = transport_->start(port_, [this](const HttpRequest& req) { return handle(req); });
  state_ = s.ok() ? ServiceState::Running : ServiceState::Degraded;
  if (!s.ok()) NX_LOG_ERROR("web", s.code(), "web transport failed to start: " + s.message());
  else
    NX_LOG_INFO("web", "web interface on http/" + std::to_string(port_));
  return s;
}

Status WebServer::stop() {
  if (transport_) transport_->stop();
  state_ = ServiceState::Stopped;
  return Status::success();
}

}  // namespace nexus::web
