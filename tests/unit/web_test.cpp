#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/EventBus.h"
#include "web/Authentication.h"
#include "web/IWebTransport.h"
#include "web/WebServer.h"

using namespace nexus::web;
using nexus::core::EventBus;

namespace {
ApiContext makeContext(int& volume, bool& reboot_called) {
  ApiContext ctx;
  ctx.status = [&] {
    return nlohmann::json{{"device_id", "SPK-1"}, {"state", "ONLINE"}, {"volume", volume}};
  };
  ctx.network = [] { return nlohmann::json{{"connected", true}, {"ip", "10.0.0.5"}}; };
  ctx.hardware = [] { return nlohmann::json{{"amplifier", "READY"}}; };
  ctx.audio = [&] { return nlohmann::json{{"volume", volume}}; };
  ctx.setVolume = [&](const nlohmann::json& b) -> std::pair<bool, std::string> {
    if (!b.contains("volume")) return {false, "missing volume"};
    volume = b["volume"].get<int>();
    return {true, "ok"};
  };
  ctx.reboot = [&]() -> std::pair<bool, std::string> {
    reboot_called = true;
    return {true, "rebooting"};
  };
  ctx.logs = [] { return std::string("log line 1\nlog line 2\n"); };
  return ctx;
}
}  // namespace

TEST(Authentication, AcceptsCorrectBearerRejectsWrong) {
  Authentication auth("s3cret");
  EXPECT_TRUE(auth.authorize("Bearer s3cret"));
  EXPECT_FALSE(auth.authorize("Bearer wrong"));
  EXPECT_FALSE(auth.authorize("s3cret"));       // missing prefix
  EXPECT_FALSE(auth.authorize(""));
}

TEST(WebServer, ServesDashboardAtRoot) {
  EventBus bus;
  int vol = 40;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  auto res = t->handle({"GET", "/", "", ""});
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.content_type, "text/html");
  EXPECT_NE(res.body.find("Nexus Speaker"), std::string::npos);
}

TEST(WebServer, RootViaHotspotServesSetupEvenWhenPaired) {
  EventBus bus;
  int vol = 40;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");  // makeContext = not in setup mode
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  // Reached at the device's own IP (LAN dashboard visitor) → read-only dashboard.
  auto lan = t->handle({"GET", "/", "", "", "192.168.1.148"});
  EXPECT_NE(lan.body.find("Nexus Speaker"), std::string::npos);  // dashboard title

  // Reached via the hotspot AP address → the interactive setup page, even though not in setup mode
  // (a paired speaker being reconfigured over Nexus-Setup). "חיבור מהיר" is the setup page heading.
  auto ap = t->handle({"GET", "/", "", "", "10.42.0.1"});
  EXPECT_NE(ap.body.find("חיבור מהיר"), std::string::npos);
  EXPECT_EQ(ap.body.find("Nexus Speaker"), std::string::npos);  // NOT the dashboard
}

TEST(WebServer, GetStatusReturnsJson) {
  EventBus bus;
  int vol = 55;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  auto res = t->handle({"GET", "/api/status", "", ""});
  ASSERT_EQ(res.status, 200);
  auto j = nlohmann::json::parse(res.body);
  EXPECT_EQ(j["volume"].get<int>(), 55);
  EXPECT_EQ(j["device_id"], "SPK-1");
}

TEST(WebServer, PostRequiresAuth) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  // No auth → 401, volume unchanged.
  auto res = t->handle({"POST", "/api/audio/volume", "{\"volume\":88}", ""});
  EXPECT_EQ(res.status, 401);
  EXPECT_EQ(vol, 30);

  // With auth → applied.
  auto ok = t->handle({"POST", "/api/audio/volume", "{\"volume\":88}", "Bearer tok"});
  EXPECT_EQ(ok.status, 200);
  EXPECT_EQ(vol, 88);
}

TEST(WebServer, SetupEndpointsOpenOnlyInSetupMode) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  bool setup = true;
  std::string joined_ssid, set_name;
  ApiContext ctx = makeContext(vol, reboot);
  ctx.status = [&] { return nlohmann::json{{"device_id", "SPK-1"}, {"setup_mode", setup}}; };
  ctx.connectWifi = [&](const nlohmann::json& b) -> std::pair<bool, std::string> {
    joined_ssid = b.value("ssid", "");
    return {true, "connecting"};
  };
  ctx.setSpeakerName = [&](const nlohmann::json& b) -> std::pair<bool, std::string> {
    set_name = b.value("name", "");
    return {true, "name set"};
  };
  WebServer web(&bus, ctx, "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  // In setup mode, the onboarding endpoints work WITHOUT an auth token for a phone that joined the
  // setup AP (Host = 10.42.0.1) — it has no token yet.
  auto j = t->handle({"POST", "/api/wifi/connect", "{\"ssid\":\"Home\",\"psk\":\"pw\"}", "",
                      "10.42.0.1"});
  EXPECT_EQ(j.status, 200);
  EXPECT_EQ(joined_ssid, "Home");
  auto n = t->handle({"POST", "/api/device/name", "{\"name\":\"Salon\"}", "", "10.42.0.1"});
  EXPECT_EQ(n.status, 200);
  EXPECT_EQ(set_name, "Salon");

  // A non-setup POST still requires auth even in setup mode, even over the hotspot.
  auto v = t->handle({"POST", "/api/audio/volume", "{\"volume\":50}", "", "10.42.0.1"});
  EXPECT_EQ(v.status, 401);

  // Once provisioned (setup_mode false), even the onboarding endpoints require auth.
  setup = false;
  auto j2 = t->handle({"POST", "/api/wifi/connect", "{\"ssid\":\"X\"}", "", "10.42.0.1"});
  EXPECT_EQ(j2.status, 401);
}

// setup_mode is cleared by pairing, not by connectivity, so it stays true after the speaker joins
// the real LAN. Exempting the onboarding endpoints on that flag alone left unauthenticated network
// reconfiguration open to every host on the LAN: anyone could repoint the speaker at another SSID.
// The exemption must additionally require that the request actually arrived over the setup AP.
TEST(WebServer, SetupEndpointsAreNotOpenToLanVisitorsInSetupMode) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  std::string joined_ssid;
  ApiContext ctx = makeContext(vol, reboot);
  ctx.status = [&] { return nlohmann::json{{"device_id", "SPK-1"}, {"setup_mode", true}}; };
  ctx.connectWifi = [&](const nlohmann::json& b) -> std::pair<bool, std::string> {
    joined_ssid = b.value("ssid", "");
    return {true, "connecting"};
  };
  WebServer web(&bus, ctx, "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  // A LAN visitor (not on the setup AP) with no token must be refused, even in setup mode.
  auto lan = t->handle({"POST", "/api/wifi/connect", "{\"ssid\":\"Attacker\",\"psk\":\"x\"}", "",
                        "192.168.1.50"});
  EXPECT_EQ(lan.status, 401) << "unauthenticated LAN client reconfigured the network";
  EXPECT_TRUE(joined_ssid.empty()) << "the join actually executed";

  // The same request WITH a valid token is fine — this is normal authenticated administration.
  auto authed = t->handle({"POST", "/api/wifi/connect", "{\"ssid\":\"Home\",\"psk\":\"x\"}",
                           "Bearer tok", "192.168.1.50"});
  EXPECT_EQ(authed.status, 200);
  EXPECT_EQ(joined_ssid, "Home");
}

// A speaker on Ethernet never raises the setup AP, so onboarding happens over the LAN at the
// device's own IP. There the /api/wifi/connect exemption does not apply (it is keyed to the hotspot
// address), so the page must authenticate itself. It could not: the served HTML carried no token,
// every join POST came back 401, and the page rendered that as "החיבור נכשל" — indistinguishable
// from a wrong Wi-Fi password, for a password that was never checked. The device serves this page,
// so it embeds its own token; the LAN exemption stays closed for everyone else.
TEST(WebServer, SetupPageCarriesTokenSoOnboardingWorksOverTheLan) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  std::string joined_ssid;
  ApiContext ctx = makeContext(vol, reboot);
  ctx.status = [&] { return nlohmann::json{{"device_id", "SPK-1"}, {"setup_mode", true}}; };
  ctx.connectWifi = [&](const nlohmann::json& b) -> std::pair<bool, std::string> {
    joined_ssid = b.value("ssid", "");
    return {true, "connecting"};
  };
  WebServer web(&bus, ctx, "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  // The setup page served over the LAN embeds the device's token and sends it on its POSTs.
  auto page = t->handle({"GET", "/setup", "", "", "192.168.1.142"});
  EXPECT_EQ(page.status, 200);
  EXPECT_NE(page.body.find("Bearer"), std::string::npos) << "page sends no Authorization header";
  EXPECT_NE(page.body.find("'tok'"), std::string::npos) << "device token not embedded in the page";
  EXPECT_EQ(page.body.find("%AUTH_TOKEN%"), std::string::npos) << "placeholder left unsubstituted";

  // Using that token, the join the page performs succeeds from the LAN.
  auto join = t->handle({"POST", "/api/wifi/connect", "{\"ssid\":\"Home\",\"psk\":\"pw\"}",
                         "Bearer tok", "192.168.1.142"});
  EXPECT_EQ(join.status, 200) << "onboarding over the LAN still refused";
  EXPECT_EQ(joined_ssid, "Home");
}

TEST(WebServer, PostRebootAuthorized) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  auto res = t->handle({"POST", "/api/system/reboot", "", "Bearer tok"});
  EXPECT_EQ(res.status, 200);
  EXPECT_TRUE(reboot);
}

TEST(WebServer, LogsEndpointReturnsPlainText) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());

  auto res = t->handle({"GET", "/api/system/logs", "", ""});
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.content_type, "text/plain");
  EXPECT_NE(res.body.find("log line 1"), std::string::npos);
}

TEST(WebServer, UnknownApiPathIs404) {
  EventBus bus;
  int vol = 30;
  bool reboot = false;
  WebServer web(&bus, makeContext(vol, reboot), "tok");
  web.start();
  auto* t = static_cast<StubWebTransport*>(web.transport());
  auto res = t->handle({"GET", "/api/nonexistent", "", ""});
  EXPECT_EQ(res.status, 404);
}
