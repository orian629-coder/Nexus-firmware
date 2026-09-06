#include <gtest/gtest.h>

#include <sodium.h>

#include <filesystem>
#include <memory>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "control/CommandClient.h"
#include "control/CommandServer.h"
#include "control/ICommandTransport.h"
#include "control/ILineTransport.h"
#include "core/EventBus.h"
#include "group/SpeakerRegistry.h"
#include "identity/Crypto.h"
#include "identity/DeviceIdentity.h"
#include "identity/KeyManager.h"
#include "pairing/PairingClient.h"
#include "pairing/PairingService.h"
#include "pairing/PairingTypes.h"
#include "storage/SecureStorage.h"
#include "web/StreamerApiRouter.h"

using namespace nexus;
namespace fs = std::filesystem;
namespace group = nexus::streamer::group;
namespace swc = nexus::streamer::control;
namespace swp = nexus::streamer::pairing;
namespace sweb = nexus::streamer::web;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_web_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

nexus::web::HttpRequest post(const std::string& path, const nlohmann::json& body) {
  return {"POST", path, body.dump(), ""};
}
nexus::web::HttpRequest get(const std::string& path) { return {"GET", path, "", ""}; }

struct StreamerKey {
  std::string sk_b64, pk_b64;
  StreamerKey() {
    unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    sk_b64 = identity::crypto::toBase64(std::vector<std::uint8_t>(sk, sk + sizeof(sk)));
    pk_b64 = identity::crypto::toBase64(std::vector<std::uint8_t>(pk, pk + sizeof(pk)));
  }
};

// Real speaker CommandServer, reachable via a loopback ILineTransport.
struct SpeakerRig {
  fs::path dir;
  core::EventBus bus;
  std::unique_ptr<storage::SecureStorage> secrets;
  std::unique_ptr<identity::DeviceIdentity> id;
  std::unique_ptr<config::ConfigManager> config;
  std::unique_ptr<control::CommandServer> server;
  control::StubCommandTransport* transport = nullptr;
  std::int64_t now = 1000;

  SpeakerRig(const std::string& name, const std::string& streamer_pk_b64) : dir(sandbox(name)) {
    secrets = std::make_unique<storage::SecureStorage>((dir / "secure").string());
    id = std::make_unique<identity::DeviceIdentity>((dir / "identity/factory.json").string(),
                                                    secrets.get(), &bus);
    id->start();
    config = std::make_unique<config::ConfigManager>((dir / "config.json").string(), &bus);
    config->start();
    config->update([&](config::SpeakerConfig& c) {
      c.pairing.paired = true;
      c.pairing.streamer_id = "STR-1";
      c.pairing.streamer_public_key = streamer_pk_b64;
    });
    auto t = std::make_unique<control::StubCommandTransport>();
    transport = t.get();
    server = std::make_unique<control::CommandServer>(&bus, id.get(), config.get(),
                                                      [this] { return now; }, std::move(t));
    server->start();
  }
};

class LoopbackLineTransport : public swc::ILineTransport {
 public:
  explicit LoopbackLineTransport(control::StubCommandTransport* t) : transport_(t) {}
  core::Result<std::string> request(const std::string&, int, const std::string& req) override {
    return transport_->deliver(req);
  }

 private:
  control::StubCommandTransport* transport_;
};

}  // namespace

// The web UI is served at "/".
TEST(WebRouter, ServesControlUi) {
  group::SpeakerRegistry reg;
  sweb::StreamerApiRouter router(reg, [](const group::Speaker&, const std::string&,
                                        const nlohmann::json&) {
    return core::Result<swc::CommandReply>(swc::CommandReply{});
  });
  auto res = router.route(get("/"));
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("Nexus"), std::string::npos);
  EXPECT_EQ(res.content_type.rfind("text/html", 0), 0u);
}

// The control QR encodes "http://<host>/?t=<token>" so scanning it opens the UI already authorized.
// Two things must hold, and both have a failure mode that is invisible until someone scans it:
//   • the URL is built from the request's Host header — the server may be bound to 0.0.0.0 and this
//     machine has several interfaces, so there is no single "own address" to hard-code;
//   • the base64 token is percent-encoded. A raw '+' in a query string decodes to a SPACE, which
//     would hand the browser a corrupted token that fails auth on every subsequent call.
TEST(WebRouter, ControlQrEncodesHostAndPercentEncodedToken) {
  group::SpeakerRegistry reg;
  // A token containing '+' and '/' — the characters base64 emits that are unsafe in a query string.
  nexus::web::Authentication auth("ab+cd/ef");
  sweb::StreamerApiRouter router(reg,
                                 [](const group::Speaker&, const std::string&,
                                    const nlohmann::json&) {
                                   return core::Result<swc::CommandReply>(swc::CommandReply{});
                                 },
                                 /*pairing_sender=*/nullptr, /*discoverer=*/nullptr,
                                 /*scanner=*/nullptr, /*transport=*/nullptr, /*store=*/nullptr,
                                 &auth);

  nexus::web::HttpRequest req{"GET", "/api/control-qr", "", "Bearer ab+cd/ef", "192.168.1.142:8090"};
  auto res = router.route(req);
  ASSERT_EQ(res.status, 200) << res.body;
  EXPECT_EQ(res.content_type, "image/svg+xml");
  EXPECT_NE(res.body.find("<svg"), std::string::npos);

  // Same request without the token must not hand out the QR — it IS the credential.
  nexus::web::HttpRequest anon{"GET", "/api/control-qr", "", "", "192.168.1.142:8090"};
  EXPECT_EQ(router.route(anon).status, 401) << "control QR handed out without auth";
}

// Onboarding a speaker that is not on the network yet. The route must refuse to start the sequence
// without both SSIDs: the setup AP to join, and the network to hand over. Getting that wrong parks
// the streamer's radio on a speaker's AP to deliver credentials it does not have.
TEST(WebRouter, OnboardApRequiresBothNetworksAndReportsResult) {
  group::SpeakerRegistry reg;
  nlohmann::json seen;
  sweb::StreamerApiRouter router(
      reg,
      [](const group::Speaker&, const std::string&, const nlohmann::json&) {
        return core::Result<swc::CommandReply>(swc::CommandReply{});
      },
      /*pairing_sender=*/nullptr, /*discoverer=*/nullptr, /*scanner=*/nullptr,
      /*transport=*/nullptr, /*store=*/nullptr, /*auth=*/nullptr,
      /*ap_scanner=*/[] { return nlohmann::json::array({{{"ssid", "Nexus-Setup"}}}); },
      /*ap_onboarder=*/
      [&seen](const nlohmann::json& body) -> nlohmann::json {
        seen = body;
        return {{"ok", true}, {"device_id", "SPK-NEW"}};
      });

  // Missing the target Wi-Fi: must not run the sequence at all.
  auto no_wifi = router.route(post("/api/onboard-ap", {{"ssid", "Nexus-Setup"}}));
  EXPECT_EQ(no_wifi.status, 400);
  EXPECT_TRUE(seen.is_null()) << "onboarding ran without credentials to deliver";

  // Missing the setup AP to join.
  auto no_ap = router.route(post("/api/onboard-ap", {{"wifi_ssid", "Home"}}));
  EXPECT_EQ(no_ap.status, 400);
  EXPECT_TRUE(seen.is_null());

  // Complete request: runs, and the parameters reach the onboarder intact.
  auto res = router.route(
      post("/api/onboard-ap", {{"ssid", "Nexus-Setup"}, {"wifi_ssid", "Home"}, {"wifi_psk", "pw"}}));
  EXPECT_EQ(res.status, 200) << res.body;
  EXPECT_EQ(seen.value("ssid", ""), "Nexus-Setup");
  EXPECT_EQ(seen.value("wifi_ssid", ""), "Home");
  EXPECT_EQ(seen.value("wifi_psk", ""), "pw");

  // The AP scan is reachable and reports what the radio saw.
  auto scan = router.route(get("/api/scan-setup-aps"));
  EXPECT_EQ(scan.status, 200);
  EXPECT_NE(scan.body.find("Nexus-Setup"), std::string::npos);
}

// A site is installed as a set, so the wizard connects every speaker it found in one pass. A
// partial result is the NORMAL outcome — one unit out of range must not read as "the step failed"
// — so the response is 200 with a per-speaker verdict, and the caller renders which one to chase.
TEST(WebRouter, BulkOnboardReportsEachSpeakerSeparately) {
  group::SpeakerRegistry reg;
  std::vector<std::string> attempted;
  sweb::StreamerApiRouter router(
      reg,
      [](const group::Speaker&, const std::string&, const nlohmann::json&) {
        return core::Result<swc::CommandReply>(swc::CommandReply{});
      },
      /*pairing_sender=*/nullptr, /*discoverer=*/nullptr, /*scanner=*/nullptr,
      /*transport=*/nullptr, /*store=*/nullptr, /*auth=*/nullptr, /*ap_scanner=*/nullptr,
      /*ap_onboarder=*/
      [&attempted](const nlohmann::json& b) -> nlohmann::json {
        const std::string s = b.value("ssid", "");
        attempted.push_back(s);
        // Middle speaker fails, as one out-of-range unit would.
        if (s == "Nexus-Setup-2") return {{"ok", false}, {"message", "out of range"}};
        return {{"ok", true}, {"device_id", "SPK-" + s.substr(s.size() - 1)}};
      });

  auto res = router.route(post("/api/onboard-ap-bulk",
                               {{"speakers", {"Nexus-Setup-1", "Nexus-Setup-2", "Nexus-Setup-3"}},
                                {"wifi_ssid", "Home"},
                                {"wifi_psk", "pw"}}));
  ASSERT_EQ(res.status, 200) << "a partial result must not read as total failure: " << res.body;
  const auto body = nlohmann::json::parse(res.body);
  EXPECT_EQ(body.value("total", 0), 3);
  EXPECT_EQ(body.value("connected", 0), 2);
  ASSERT_EQ(attempted.size(), 3u) << "every speaker must be attempted, not just up to the failure";
  // The failed one is identifiable, which is the whole point of per-speaker results.
  const auto& r = body["results"];
  EXPECT_FALSE(r[1].value("ok", true));
  EXPECT_EQ(r[1].value("ssid", ""), "Nexus-Setup-2");
  EXPECT_TRUE(r[0].value("ok", false));
  EXPECT_TRUE(r[2].value("ok", false));

  // Credentials to hand over are mandatory: without them the radio would join each speaker's AP
  // and deliver nothing.
  EXPECT_EQ(router.route(post("/api/onboard-ap-bulk", {{"speakers", {"Nexus-Setup-1"}}})).status,
            400);
}

// A host with no spare radio must say so, not fail in a way that reads as "no speakers found".
TEST(WebRouter, ApRoutesReportUnavailableWithoutARadio) {
  group::SpeakerRegistry reg;
  sweb::StreamerApiRouter router(reg, [](const group::Speaker&, const std::string&,
                                         const nlohmann::json&) {
    return core::Result<swc::CommandReply>(swc::CommandReply{});
  });
  EXPECT_EQ(router.route(get("/api/scan-setup-aps")).status, 501);
  EXPECT_EQ(
      router.route(post("/api/onboard-ap", {{"ssid", "Nexus-Setup"}, {"wifi_ssid", "Home"}})).status,
      501);
}

// MAC is an identification aid layered onto the list; device_id stays the key. An unknown MAC must
// be omitted rather than rendered as an empty or placeholder value.
TEST(WebRouter, SpeakerListShowsMacWhenKnownAndOmitsItOtherwise) {
  group::SpeakerRegistry reg;
  reg.upsert({"SPK-1", "Salon", "192.168.1.50", 45455});
  reg.upsert({"SPK-2", "Kitchen", "192.168.1.51", 45455});
  sweb::StreamerApiRouter router(reg, [](const group::Speaker&, const std::string&,
                                         const nlohmann::json&) {
    return core::Result<swc::CommandReply>(swc::CommandReply{});
  });
  router.setMacLookup([](const std::string& ip) -> std::string {
    return ip == "192.168.1.50" ? "b8:27:eb:11:22:33" : "";  // .51 not in the ARP table
  });

  auto res = router.route(get("/api/speakers"));
  ASSERT_EQ(res.status, 200);
  const auto body = nlohmann::json::parse(res.body);
  ASSERT_EQ(body["speakers"].size(), 2u);
  for (const auto& s : body["speakers"]) {
    if (s.value("device_id", "") == "SPK-1") {
      EXPECT_EQ(s.value("mac", ""), "b8:27:eb:11:22:33");
    } else {
      EXPECT_FALSE(s.contains("mac")) << "unknown MAC rendered instead of omitted";
    }
  }
}

// Add / list / remove speakers via the registry endpoints.
TEST(WebRouter, ManagesSpeakerRegistry) {
  group::SpeakerRegistry reg;
  sweb::StreamerApiRouter router(reg, [](const group::Speaker&, const std::string&,
                                        const nlohmann::json&) {
    return core::Result<swc::CommandReply>(swc::CommandReply{});
  });

  auto add = router.route(post("/api/speakers", {{"device_id", "SPK-1"}, {"name", "Salon"},
                                                 {"host", "10.0.0.5"}}));
  EXPECT_EQ(add.status, 200);
  EXPECT_EQ(reg.size(), 1u);

  auto list = nlohmann::json::parse(router.route(get("/api/speakers")).body);
  ASSERT_EQ(list["speakers"].size(), 1u);
  EXPECT_EQ(list["speakers"][0]["name"], "Salon");

  auto rm = router.route(post("/api/speakers/remove", {{"device_id", "SPK-1"}}));
  EXPECT_EQ(rm.status, 200);
  EXPECT_EQ(reg.size(), 0u);
}

// The full path: UI action → router → real CommandClient → real speaker CommandServer, effect
// persisted. This proves the browser control actually drives the speaker.
TEST(WebRouter, VolumeActionReachesRealSpeaker) {
  StreamerKey key;
  SpeakerRig rig("vol", key.pk_b64);
  const std::string device_id = rig.id->deviceId();

  group::SpeakerRegistry reg;
  group::Speaker s;
  s.device_id = device_id;
  s.name = "Salon";
  s.host = "loopback";
  reg.upsert(s);

  LoopbackLineTransport line(rig.transport);
  int seq = 0;
  sweb::StreamerApiRouter router(reg, [&](const group::Speaker& target, const std::string& command,
                                         const nlohmann::json& payload) {
    swc::CommandClient client(
        line, swc::CommandSigner(key.sk_b64), target.host, target.control_port);
    return client.send("cmd-" + std::to_string(++seq), target.device_id, command, payload,
                       rig.now + 30, rig.now);
  });

  auto res = router.route(post("/api/volume", {{"speaker", device_id}, {"volume", 42}}));
  EXPECT_EQ(res.status, 200);
  auto j = nlohmann::json::parse(res.body);
  EXPECT_TRUE(j["ok"]);
  EXPECT_EQ(rig.config->get().audio.volume, 42);  // persisted on the real speaker

  // GET_STATUS round-trips real data back to the UI.
  auto st = router.route(post("/api/status", {{"speaker", device_id}}));
  auto sj = nlohmann::json::parse(st.body);
  EXPECT_TRUE(sj["ok"]);
  EXPECT_EQ(sj["data"]["volume"], 42);
}

// Full pairing path: UI POST /api/pair → router → real PairingClient → real PairingService, and on
// success the speaker is added to the registry.
TEST(WebRouter, PairAddsSpeakerViaRealPairingService) {
  // Real speaker pairing stack, reachable via a loopback transport reproducing the speaker's
  // raw-JSON pairing handler (sealed_wifi/signature key mapping).
  struct PairRig : public swc::ILineTransport {
    fs::path dir;
    core::EventBus bus;
    std::unique_ptr<storage::SecureStorage> secrets;
    std::unique_ptr<identity::KeyManager> keys;
    std::unique_ptr<config::ConfigManager> config;
    std::unique_ptr<pairing::PairingService> svc;
    std::int64_t now = 1100;
    PairRig() : dir(sandbox("pair")) {
      secrets = std::make_unique<storage::SecureStorage>((dir / "secure").string());
      keys = std::make_unique<identity::KeyManager>(secrets.get());
      keys->generateIfMissing();
      config = std::make_unique<config::ConfigManager>((dir / "config.json").string(), &bus);
      config->start();
      svc = std::make_unique<pairing::PairingService>(&bus, keys.get(), config.get(), secrets.get());
      svc->start();
    }
    core::Result<std::string> request(const std::string&, int, const std::string& raw) override {
      pairing::PairingRequest req;
      auto j = nlohmann::json::parse(raw);
      req.streamer_id = j.value("streamer_id", "");
      req.streamer_public_key = j.value("streamer_public_key", "");
      req.site_id = j.value("site_id", "");
      req.initial_speaker_name = j.value("initial_speaker_name", "");
      req.setup_code = j.value("setup_code", "");
      req.sealed_wifi_b64 = j.value("sealed_wifi", "");
      req.signature_b64 = j.value("signature", "");
      auto res = svc->processRequest(req, now);
      if (!res.ok())
        return nlohmann::json{{"ok", false}, {"message", res.status().message()}}.dump();
      return nlohmann::json{{"ok", true}, {"device_id", "SPK-PAIRED"}}.dump();
    }
  } rig;
  rig.svc->beginSetupMode("123456", 1000, 300);

  StreamerKey key;
  group::SpeakerRegistry reg;
  auto pairing_sender = [&](const std::string& host, swp::PairingParams params) {
    params.streamer_id = "STR-TEST";
    params.streamer_public_key = key.pk_b64;
    params.streamer_secret_key = key.sk_b64;
    swp::PairingClient client(rig, host, 45455);
    return client.pair(params);
  };
  sweb::StreamerApiRouter router(
      reg,
      [](const group::Speaker&, const std::string&, const nlohmann::json&) {
        return core::Result<swc::CommandReply>(swc::CommandReply{});
      },
      pairing_sender);

  auto res = router.route(post("/api/pair", {{"host", "loopback"},
                                             {"name", "Salon"},
                                             {"setup_code", "123456"},
                                             {"box_public_key", rig.keys->boxPublicKeyBase64().value()},
                                             {"wifi_ssid", "NexusLab"},
                                             {"wifi_psk", "secret-pw"}}));
  EXPECT_EQ(res.status, 200) << res.body;
  auto j = nlohmann::json::parse(res.body);
  EXPECT_TRUE(j["ok"]);
  EXPECT_EQ(j["device_id"], "SPK-PAIRED");
  // Speaker is now in the registry and the speaker persisted our key.
  EXPECT_EQ(reg.size(), 1u);
  EXPECT_TRUE(rig.config->get().pairing.paired);
  EXPECT_EQ(rig.config->get().pairing.streamer_public_key, key.pk_b64);
}

// Wrong setup code → router returns 400 and no speaker is added.
TEST(WebRouter, PairWithWrongCodeRejected) {
  group::SpeakerRegistry reg;
  auto pairing_sender = [&](const std::string&, swp::PairingParams) {
    return core::Result<swp::PairingReply>(swp::PairingReply{false, "bad code", ""});
  };
  sweb::StreamerApiRouter router(
      reg,
      [](const group::Speaker&, const std::string&, const nlohmann::json&) {
        return core::Result<swc::CommandReply>(swc::CommandReply{});
      },
      pairing_sender);
  auto res = router.route(post("/api/pair", {{"host", "h"},
                                             {"setup_code", "000000"},
                                             {"box_public_key", "x"}}));
  EXPECT_EQ(res.status, 400);
  EXPECT_EQ(reg.size(), 0u);
}

// /api/discover returns the discovered speakers when a discoverer is injected.
TEST(WebRouter, DiscoverListsSpeakers) {
  group::SpeakerRegistry reg;
  auto discoverer = []() {
    nexus::streamer::discovery::DiscoveredSpeaker s;
    s.device_id = "SPK-FOUND";
    s.host = "192.168.1.77";
    s.box_public_key = "boxkey-b64";
    s.control_port = 45455;
    s.setup_mode = true;
    return core::Result<std::vector<nexus::streamer::discovery::DiscoveredSpeaker>>(
        std::vector<nexus::streamer::discovery::DiscoveredSpeaker>{s});
  };
  sweb::StreamerApiRouter router(
      reg,
      [](const group::Speaker&, const std::string&, const nlohmann::json&) {
        return core::Result<swc::CommandReply>(swc::CommandReply{});
      },
      nullptr, discoverer);

  auto res = router.route(get("/api/discover"));
  EXPECT_EQ(res.status, 200);
  auto j = nlohmann::json::parse(res.body);
  ASSERT_EQ(j["speakers"].size(), 1u);
  EXPECT_EQ(j["speakers"][0]["device_id"], "SPK-FOUND");
  EXPECT_EQ(j["speakers"][0]["host"], "192.168.1.77");
  EXPECT_EQ(j["speakers"][0]["box_public_key"], "boxkey-b64");
  EXPECT_TRUE(j["speakers"][0]["setup_mode"]);
}

// Without a discoverer injected, /api/discover reports 501 (e.g. on a dev host with no Avahi).
TEST(WebRouter, DiscoverUnavailableWithoutBackend) {
  group::SpeakerRegistry reg;
  sweb::StreamerApiRouter router(reg, [](const group::Speaker&, const std::string&,
                                         const nlohmann::json&) {
    return core::Result<swc::CommandReply>(swc::CommandReply{});
  });
  auto res = router.route(get("/api/discover"));
  EXPECT_EQ(res.status, 501);
}

TEST(WebRouter, UnknownSpeakerIs404) {
  group::SpeakerRegistry reg;
  sweb::StreamerApiRouter router(reg, [](const group::Speaker&, const std::string&,
                                        const nlohmann::json&) {
    return core::Result<swc::CommandReply>(swc::CommandReply{});
  });
  auto res = router.route(post("/api/volume", {{"speaker", "SPK-NOPE"}, {"volume", 10}}));
  EXPECT_EQ(res.status, 404);
}

namespace {

// Router over one registered speaker that records the command it was asked to send. Used by the
// diagnostic/system tests below, which care about *which* command reached the transport rather
// than about what the speaker replied.
struct RecordingRig {
  group::SpeakerRegistry reg;
  std::shared_ptr<std::string> last = std::make_shared<std::string>();
  std::shared_ptr<nlohmann::json> last_payload = std::make_shared<nlohmann::json>();
  sweb::StreamerApiRouter router;

  RecordingRig()
      : router(reg, [l = last, p = last_payload](const group::Speaker&, const std::string& cmd,
                                                 const nlohmann::json& payload) {
          *l = cmd;
          *p = payload;
          return core::Result<swc::CommandReply>(swc::CommandReply{});
        }) {
    group::Speaker s;
    s.device_id = "SPK-1";
    s.host = "127.0.0.1";
    reg.upsert(s);
  }
};

}  // namespace

// The diagnostics routes must map to their own commands. RUN_CALIBRATION in particular is distinct
// from RUN_MEASUREMENT (/api/measure): measurement reports distance, calibration applies a fix.
TEST(WebRouter, DiagnosticRoutesMapToTheirCommands) {
  RecordingRig rig;

  EXPECT_EQ(rig.router.route(post("/api/calibrate", {{"speaker", "SPK-1"}})).status, 200);
  EXPECT_EQ(*rig.last, "RUN_CALIBRATION");

  EXPECT_EQ(rig.router.route(post("/api/self-test", {{"speaker", "SPK-1"}})).status, 200);
  EXPECT_EQ(*rig.last, "RUN_SELF_TEST");

  EXPECT_EQ(rig.router.route(post("/api/audio-test", {{"speaker", "SPK-1"}})).status, 200);
  EXPECT_EQ(*rig.last, "RUN_AUDIO_TEST");
}

// Optional tuning parameters pass through; absent ones are NOT defaulted here, so the speaker's own
// defaults stay the single source of truth.
TEST(WebRouter, DiagnosticPayloadPassesThroughOnlySuppliedKeys) {
  RecordingRig rig;
  rig.router.route(post("/api/audio-test", {{"speaker", "SPK-1"}, {"frequency", 440}}));
  EXPECT_EQ((*rig.last_payload)["frequency"], 440);
  EXPECT_FALSE(rig.last_payload->contains("duration_s"));
}

// Every destructive route refuses to send anything without confirm:true. This is the guard that
// matters most: without it, a stray LAN request could reboot or wipe a speaker.
TEST(WebRouter, DestructiveRoutesRequireConfirmation) {
  for (const char* path : {"/api/reboot", "/api/update", "/api/reset-network",
                           "/api/factory-reset"}) {
    RecordingRig rig;
    auto res = rig.router.route(post(path, {{"speaker", "SPK-1"}}));
    EXPECT_EQ(res.status, 400) << path;
    EXPECT_TRUE(rig.last->empty()) << path << " sent a command without confirmation";
  }
}

TEST(WebRouter, ConfirmedDestructiveRoutesSendTheirCommands) {
  RecordingRig rig;

  EXPECT_EQ(rig.router.route(post("/api/reboot", {{"speaker", "SPK-1"}, {"confirm", true}})).status,
            200);
  EXPECT_EQ(*rig.last, "REBOOT");

  EXPECT_EQ(
      rig.router.route(post("/api/reset-network", {{"speaker", "SPK-1"}, {"confirm", true}})).status,
      200);
  EXPECT_EQ(*rig.last, "RESET_NETWORK");

  EXPECT_EQ(rig.router.route(post("/api/update", {{"speaker", "SPK-1"}, {"confirm", true},
                                                  {"url", "http://x/f.bin"}})).status,
            200);
  EXPECT_EQ(*rig.last, "UPDATE_SOFTWARE");
  EXPECT_EQ((*rig.last_payload)["url"], "http://x/f.bin");
}

// Factory reset erases pairing, so confirm:true alone is deliberately not enough.
TEST(WebRouter, FactoryResetNeedsUnpairAcknowledgement) {
  RecordingRig rig;

  auto half = rig.router.route(post("/api/factory-reset", {{"speaker", "SPK-1"}, {"confirm", true}}));
  EXPECT_EQ(half.status, 400);
  EXPECT_TRUE(rig.last->empty()) << "factory reset fired without the unpair acknowledgement";

  auto full = rig.router.route(post("/api/factory-reset", {{"speaker", "SPK-1"},
                                                           {"confirm", true},
                                                           {"acknowledge_unpair", "yes"}}));
  EXPECT_EQ(full.status, 200);
  EXPECT_EQ(*rig.last, "FACTORY_RESET");
}
