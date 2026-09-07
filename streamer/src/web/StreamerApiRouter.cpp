#include "web/StreamerApiRouter.h"

#include "control/Command.h"      // nexus::control::cmd:: name constants
#include "web/QrEncoder.h"         // setup QR for the control UI
#include "web/StreamerFrontend.h"  // controlUiHtml()

namespace nexus::streamer::web {

using nexus::web::HttpRequest;
using nexus::web::HttpResponse;

namespace {
HttpResponse json(int status, const nlohmann::json& body) {
  return {status, "application/json", body.dump(), {}};
}
HttpResponse ok(const nlohmann::json& body) { return json(200, body); }
HttpResponse notFound() { return json(404, {{"error", "not found"}}); }
HttpResponse badRequest(const std::string& m) { return json(400, {{"error", m}}); }
}  // namespace

HttpResponse StreamerApiRouter::route(const HttpRequest& req) const {
  // The UI shell itself is open so a browser can load the page and prompt for the token; every
  // /api/ route below it requires the bearer token, GETs included. /api/speakers exposes the site's
  // topology and /api/pair carries a plaintext Wi-Fi PSK, so "reads are harmless on a trusted LAN"
  // does not hold here the way it does for a single speaker's dashboard.
  const bool is_api = req.path.rfind("/api/", 0) == 0;
  if (is_api && auth_ && !auth_->authorize(req.auth)) {
    return {401,
            "application/json",
            nlohmann::json{{"error", "unauthorized"}}.dump(),
            {{"WWW-Authenticate", "Bearer"}}};
  }

  if (req.method == "GET") return handleGet(req.path, req.host);
  if (req.method == "POST") return handlePost(req);
  return json(405, {{"error", "method not allowed"}});
}

HttpResponse StreamerApiRouter::handleGet(const std::string& path, const std::string& host) const {
  if (path == "/" || path == "/index.html") {
    return {200, "text/html; charset=utf-8", controlUiHtml(), {}};
  }
  if (path == "/api/speakers") {
    // Addressing comes from the registry (the streamer owns it); every reported value and the
    // online flag come from the state store, which is written only from speaker replies. The UI
    // therefore cannot render a number that no speaker ever confirmed.
    nlohmann::json arr = nlohmann::json::array();
    const auto now = store_ ? store_->now() : std::chrono::steady_clock::now();
    for (const auto& s : registry_.list()) {
      nlohmann::json entry{{"device_id", s.device_id},
                           {"name", s.name},
                           {"host", s.host},
                           {"control_port", s.control_port}};
      // MAC is shown to help identify a unit physically; device_id remains the key. It is absent
      // whenever the ARP entry is not cached or the speaker is off-segment, so the UI treats an
      // empty value as "unknown" rather than "no MAC".
      if (mac_lookup_ && !s.host.empty()) {
        const std::string mac = mac_lookup_(s.host);
        if (!mac.empty()) entry["mac"] = mac;
      }
      if (store_) {
        if (auto st = store_->get(s.device_id)) {
          entry.update(st->toJson(now));
        } else {
          // Registered but never yet heard from: explicitly not online and not confirmed, rather
          // than defaulting to a cheerful "online".
          entry["online"] = false;
          entry["confirmed"] = false;
        }
      }
      arr.push_back(std::move(entry));
    }
    return ok({{"speakers", arr}});
  }
  if (path == "/api/discover") {
    if (!discoverer_) return json(501, {{"error", "discovery not available"}});
    auto found = discoverer_();
    if (!found.ok()) return json(502, {{"error", found.status().message()}});
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : found.value()) {
      arr.push_back({{"device_id", s.device_id},
                     {"host", s.host},
                     {"box_public_key", s.box_public_key},
                     {"control_port", s.control_port},
                     {"setup_mode", s.setup_mode}});
    }
    return ok({{"speakers", arr}});
  }
  if (path == "/api/scan-network") {
    if (!scanner_) return json(501, {{"error", "network scan not available"}});
    return ok({{"speakers", scanner_()}});
  }
  // Speakers that have never been on a network cannot answer mDNS or the subnet sweep — they are
  // only visible as the setup AP they broadcast. This scans the streamer's spare radio for them.
  if (path == "/api/scan-setup-aps") {
    if (!ap_scanner_) return json(501, {{"error", "no spare wifi radio for setup scanning"}});
    return ok({{"access_points", ap_scanner_()}});
  }
  if (path == "/api/zones") {
    if (!zones_) return json(501, {{"error", "zones not available"}});
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& z : zones_->list()) {
      arr.push_back({{"zone_id", z.zone_id}, {"name", z.name}, {"members", z.members}});
    }
    return ok({{"zones", arr}});
  }
  if (path == "/api/sources") {
    // Capture devices are enumerated live rather than cached: the user can plug in an interface or
    // install BlackHole while the streamer is running, and a cached list would keep hiding it.
    // This runs on the web thread — enumeration spawns a helper process and must never be done on
    // the audio thread.
    nlohmann::json devices = nlohmann::json::array();
    for (const auto& d : sources::listCaptureDevices()) {
      devices.push_back({{"id", d.id}, {"name", d.name}, {"loopback", d.loopback}});
    }
    return ok({{"kinds", sources::availableKinds()}, {"devices", std::move(devices)}});
  }

  if (path == "/api/dsp") {
    if (!dsp_get_) return json(501, {{"error", "dsp not available"}});
    return ok(dsp_get_());
  }

  if (path == "/api/levels") {
    if (!levels_) return json(501, {{"error", "levels not available"}});
    return ok(levels_());
  }
  // Zero-touch provisioning window: whether the streamer is currently accepting a speaker's
  // unauthenticated pairing offer, and how long that stays true. GET is read-only status; the web-UI
  // toggle drives it via the POST branch below. Both branches read `now` from the same injected
  // clock_ so isOpen()/secondsRemaining() can never disagree within one request.
  if (path == "/api/provisioning-window") {
    if (!window_) return json(503, {{"error", "provisioning unavailable"}});
    const auto now = clock_();
    return json(200, {{"open", window_->isOpen(now)},
                      {"seconds_remaining", window_->secondsRemaining(now)}});
  }
  // Control-UI QR: scanning it opens this UI already authenticated, so a phone needs neither the
  // address nor the token typed in. The token is persisted in the streamer config (generated once
  // on first run), so a QR printed today still works after a restart.
  //
  // NOTE: this QR is a CREDENTIAL — anyone who photographs it gets full control of the system.
  // That is the deliberate trade the operator asked for; keep it off public displays.
  //
  // The URL is built from the request's own Host header rather than a configured address: the
  // server may be bound to 0.0.0.0, and a host with several interfaces (this streamer has both
  // Ethernet and Wi-Fi) has no single canonical IP. Whatever address reached us is, by definition,
  // an address that works.
  if (path == "/api/control-qr") {
    if (host.empty()) return json(503, {{"error", "cannot determine own address"}});
    std::string url = "http://" + host + "/?t=";
    // The token is base64, which contains '+' and '/' — both of which change meaning inside a query
    // string ('+' decodes to a space). Percent-encode everything that is not URL-safe.
    for (unsigned char c : (auth_ ? auth_->token() : std::string())) {
      const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
      if (unreserved) {
        url += static_cast<char>(c);
      } else {
        static const char* kHex = "0123456789ABCDEF";
        url += '%';
        url += kHex[c >> 4];
        url += kHex[c & 0x0F];
      }
    }
    try {
      return {200, "image/svg+xml", nexus::web::QrEncoder::toSvg(url), {}};
    } catch (const std::exception& e) {
      return json(500, {{"error", e.what()}});
    }
  }
  return notFound();
}

HttpResponse StreamerApiRouter::handlePost(const HttpRequest& req) const {
  nlohmann::json body = nlohmann::json::object();
  if (!req.body.empty()) {
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
      return badRequest(std::string("bad json: ") + e.what());
    }
  }

  const std::string& p = req.path;

  // ── registry management ──
  if (p == "/api/speakers") {
    if (!body.contains("device_id") || !body.contains("host")) {
      return badRequest("device_id and host required");
    }
    group::Speaker s;
    s.device_id = body.value("device_id", "");
    s.name = body.value("name", s.device_id);
    s.host = body.value("host", "");
    s.control_port = body.value("control_port", 45455);
    const bool added = registry_.upsert(s);
    if (persist_) persist_();
    return ok({{"ok", true}, {"added", added}});
  }
  if (p == "/api/speakers/remove") {
    const bool removed = registry_.remove(body.value("device_id", ""));
    if (persist_) persist_();
    return ok({{"ok", removed}});
  }

  // ── full device control: proxy to the speaker's own web API ──
  //
  // POST /api/device  {"speaker":"SPK-…","method":"GET|POST","path":"/api/…","body":{…}}
  //
  // One generic route rather than a mirror of every speaker endpoint: the speaker's API is the
  // source of truth for what a speaker can do, and duplicating it here would mean this list going
  // stale the moment the firmware gains a feature. The streamer's own auth already gates this, and
  // the path is restricted to /api/ so it cannot be used to fetch arbitrary URLs from the speaker.
  if (p == "/api/device") {
    if (!speaker_http_) return json(501, {{"error", "device proxy not available"}});
    const std::string id = body.value("speaker", "");
    if (id.empty()) return badRequest("speaker required");
    auto target = registry_.get(id);
    if (!target) return json(404, {{"error", "unknown speaker: " + id}});

    const std::string method = body.value("method", "GET");
    const std::string path = body.value("path", "");
    if (path.rfind("/api/", 0) != 0) return badRequest("path must start with /api/");

    std::string payload;
    if (body.contains("body") && !body["body"].is_null()) payload = body["body"].dump();

    const auto [status, text] = speaker_http_(*target, method, path, payload);
    if (status == 0) {
      return json(502, {{"ok", false}, {"message", "speaker unreachable"}});
    }
    // Pass the speaker's own JSON straight through so the UI sees exactly what the device said,
    // rather than a re-shaped copy that could drift from it.
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    return json(status, {{"ok", status >= 200 && status < 300},
                         {"data", parsed.is_discarded() ? nlohmann::json(text) : parsed}});
  }

  // ── zones ──
  if (p.rfind("/api/zones", 0) == 0) {
    if (!zones_) return json(501, {{"error", "zones not available"}});

    if (p == "/api/zones") {
      auto z = zones_->create(body.value("name", ""));
      if (!z.ok()) return json(400, {{"error", z.status().message()}});
      if (persist_) persist_();
      return ok({{"ok", true}, {"zone_id", z.value().zone_id}, {"name", z.value().name}});
    }
    if (p == "/api/zones/remove") {
      const bool removed = zones_->remove(body.value("zone_id", ""));
      if (persist_) persist_();
      return ok({{"ok", removed}});
    }
    if (p == "/api/zones/rename") {
      auto s = zones_->rename(body.value("zone_id", ""), body.value("name", ""));
      if (!s.ok()) return json(404, {{"error", s.message()}});
      if (persist_) persist_();
      return ok({{"ok", true}});
    }
    if (p == "/api/zones/members") {
      const std::string zone_id = body.value("zone_id", "");
      const std::string device_id = body.value("device_id", "");
      const bool add = body.value("add", true);
      auto s = add ? zones_->addMember(zone_id, device_id)
                   : zones_->removeMember(zone_id, device_id);
      if (!s.ok()) return json(400, {{"ok", false}, {"error", s.message()}});
      if (persist_) persist_();
      return ok({{"ok", true}});
    }
    if (p == "/api/zones/play") {
      if (!play_zone_) return json(501, {{"error", "playback not available"}});
      sources::SourceSpec spec;
      spec.kind = body.value("kind", "file");
      spec.uri = body.value("uri", "");
      auto s = play_zone_(body.value("zone_id", ""), spec);
      if (!s.ok()) return json(400, {{"ok", false}, {"message", s.message()}});
      return ok({{"ok", true}});
    }
    // Change WHICH speakers the running stream goes to, without restarting it. Calling
    // /api/zones/play again would rebuild the source — on macOS that spawns a new capture process —
    // and the restart is audible as a gap.
    if (p == "/api/zones/retarget") {
      if (!retarget_zone_) return json(501, {{"error", "retarget not available"}});
      auto s = retarget_zone_(body.value("zone_id", ""));
      if (!s.ok()) return json(400, {{"ok", false}, {"message", s.message()}});
      return ok({{"ok", true}});
    }
    if (p == "/api/zones/volume") {
      if (!body.contains("volume")) return badRequest("volume required");
      // Applied per member, and each member's CONFIRMED reply is reported back. The zone has no
      // volume of its own — showing one would mean inventing a value no speaker reported.
      nlohmann::json results = nlohmann::json::array();
      bool all_ok = true;
      for (const auto& sp : zones_->members(body.value("zone_id", ""))) {
        auto reply = sender_(sp, nexus::control::cmd::kSetVolume, {{"volume", body["volume"]}});
        const bool ok_one = reply.ok() && reply.value().ok;
        all_ok = all_ok && ok_one;
        nlohmann::json entry{{"device_id", sp.device_id}, {"ok", ok_one}};
        if (!reply.ok()) {
          entry["message"] = reply.status().message();
        } else {
          entry["message"] = reply.value().message;
          if (reply.value().data.contains("volume")) entry["volume"] = reply.value().data["volume"];
        }
        results.push_back(std::move(entry));
      }
      return ok({{"ok", all_ok}, {"results", results}});
    }
    return notFound();
  }

  // ── master DSP (global stage: applies to every speaker at once) ──
  //
  // A partial body updates only the named fields, so the UI can move one slider without having to
  // resend the whole 32-band curve and risk clobbering a concurrent change.
  if (p == "/api/dsp") {
    if (!dsp_set_) return json(501, {{"error", "dsp not available"}});
    nlohmann::json applied = dsp_set_(body);
    if (persist_) persist_();
    return ok(std::move(applied));
  }

  // ── pair a new speaker (setup mode) then add it to the registry on success ──
  // Onboard a speaker that is not on the network yet: join the setup AP it broadcasts, run the
  // normal pairing handshake across that link (which is what carries the sealed Wi-Fi credentials),
  // then drop the AP. Distinct from /api/pair, which assumes the speaker is already reachable.
  //
  // The whole sequence is one request because the intermediate state — the streamer's radio parked
  // on a speaker's AP — must not outlive the operation. Splitting it across calls would leave the
  // radio stranded there if the browser closed mid-flow.
  // Onboard SEVERAL speakers in one pass. A site has many speakers and they are installed together,
  // so running a separate wizard per unit is the wrong shape: the operator picks the home Wi-Fi
  // once and every speaker found gets it.
  //
  // Each speaker is reported individually, because a partial result is the normal outcome — one
  // unit out of range or slow to boot must not read as "the whole step failed", and the operator
  // needs to know WHICH one to go look at. They are onboarded one at a time by necessity: the
  // streamer has a single spare radio and must be associated with one setup AP at a time.
  if (p == "/api/onboard-ap-bulk") {
    if (!ap_onboarder_) return json(501, {{"error", "no spare wifi radio for onboarding"}});
    if (!body.contains("speakers") || !body["speakers"].is_array()) {
      return badRequest("speakers array required");
    }
    if (body.value("wifi_ssid", "").empty()) return badRequest("wifi_ssid required");

    nlohmann::json results = nlohmann::json::array();
    int ok_count = 0;
    for (const auto& sp : body["speakers"]) {
      const std::string ssid = sp.is_string() ? sp.get<std::string>() : sp.value("ssid", "");
      if (ssid.empty()) continue;
      nlohmann::json one{{"ssid", ssid},
                         {"wifi_ssid", body["wifi_ssid"]},
                         {"wifi_psk", body.value("wifi_psk", "")}};
      if (sp.is_object() && sp.contains("name")) one["name"] = sp["name"];
      auto r = ap_onboarder_(one);
      const bool good = r.value("ok", false);
      if (good) ++ok_count;
      results.push_back({{"ssid", ssid},
                         {"ok", good},
                         {"device_id", r.value("device_id", "")},
                         {"message", r.value("message", "")}});
    }
    // 200 even when some failed: the per-speaker results are the answer, and the caller renders
    // them. A non-2xx here would make a partially successful run look like a total failure.
    return ok({{"results", results},
               {"connected", ok_count},
               {"total", results.size()}});
  }

  if (p == "/api/onboard-ap") {
    if (!ap_onboarder_) return json(501, {{"error", "no spare wifi radio for onboarding"}});
    if (body.value("ssid", "").empty()) return badRequest("ssid required");
    if (body.value("wifi_ssid", "").empty()) return badRequest("wifi_ssid required");
    auto res = ap_onboarder_(body);
    return json(res.value("ok", false) ? 200 : 400, res);
  }

  // Zero-touch provisioning window toggle. {"open":true[,"ttl":600]} opens (or extends) the window;
  // {"open":false} closes it early. `body` was already parsed (and malformed JSON already rejected)
  // above, so this reuses it rather than re-parsing req.body — the one-parse-per-request rule every
  // other POST branch in this function follows.
  if (p == "/api/provisioning-window") {
    if (!window_) return json(503, {{"error", "provisioning unavailable"}});
    if (!body.contains("open") || !body["open"].is_boolean()) return badRequest("missing 'open'");
    const auto now = clock_();
    if (body["open"].get<bool>()) {
      int ttl = 600;
      if (body.contains("ttl")) {
        if (!body["ttl"].is_number_integer()) return badRequest("ttl must be an integer");
        ttl = body["ttl"].get<int>();
      }
      window_->open(now, ttl);
    } else {
      window_->close();
    }
    return json(200, {{"open", window_->isOpen(now)},
                      {"seconds_remaining", window_->secondsRemaining(now)}});
  }

  if (p == "/api/pair") {
    if (!pairing_sender_) return json(501, {{"error", "pairing not available"}});
    const std::string host = body.value("host", "");
    if (host.empty()) return badRequest("host required");
    if (!body.contains("setup_code") || !body.contains("box_public_key")) {
      return badRequest("setup_code and box_public_key required");
    }
    nexus::streamer::pairing::PairingParams params;
    params.site_id = body.value("site_id", "default");
    params.initial_speaker_name = body.value("name", host);
    params.setup_code = body.value("setup_code", "");
    params.wifi_ssid = body.value("wifi_ssid", "");
    params.wifi_psk = body.value("wifi_psk", "");
    params.speaker_box_public_key = body.value("box_public_key", "");
    // streamer_id / keys are filled in by the injected sender (it holds the identity).

    auto reply = pairing_sender_(host, params);
    if (!reply.ok()) return json(502, {{"ok", false}, {"message", reply.status().message()}});
    if (!reply.value().ok) {
      return json(400, {{"ok", false}, {"message", reply.value().message}});
    }
    // Paired: register the speaker so it appears in the control list.
    group::Speaker s;
    s.device_id = reply.value().device_id;
    s.name = body.value("name", s.device_id);
    s.host = host;
    s.control_port = body.value("control_port", 45455);
    registry_.upsert(s);
    if (persist_) persist_();  // a newly paired speaker must survive a restart
    return ok({{"ok", true}, {"device_id", s.device_id}});
  }

  // ── speaker-scoped commands ──
  if (p == "/api/status") return sendToSpeaker(body, nexus::control::cmd::kGetStatus, {});
  // Make ONE speaker emit a short tone, so the installer can tell which physical unit in the room
  // a row in this list is. These speakers have no addressable indicator LED, so sound is the only
  // identification channel available.
  if (p == "/api/identify") return sendToSpeaker(body, nexus::control::cmd::kRunAudioTest, {});

  // ── Bulk actions ──────────────────────────────────────────────────────────
  // The point of the system is many speakers, so the everyday operations must apply to a selection
  // rather than forcing the operator to repeat themselves per unit. Each speaker is reported
  // separately: one unreachable speaker must not make the whole action look failed, and the
  // operator needs to know which one to chase.
  if (p == "/api/bulk") {
    if (!body.contains("speakers") || !body["speakers"].is_array()) {
      return badRequest("speakers array required");
    }
    const std::string action = body.value("action", "");
    nlohmann::json payload = nlohmann::json::object();
    std::string command;
    if (action == "volume") {
      if (!body.contains("volume")) return badRequest("volume required");
      command = nexus::control::cmd::kSetVolume;
      payload["volume"] = body["volume"];
    } else if (action == "mute") {
      if (!body.contains("muted")) return badRequest("muted required");
      command = nexus::control::cmd::kSetMute;
      payload["muted"] = body["muted"];
    } else if (action == "eq") {
      if (!body.contains("eq_profile")) return badRequest("eq_profile required");
      command = nexus::control::cmd::kSetEq;
      payload["eq_profile"] = body["eq_profile"];
    } else if (action == "reboot") {
      command = nexus::control::cmd::kReboot;
    } else {
      return badRequest("unknown action: " + action);
    }

    nlohmann::json results = nlohmann::json::array();
    int ok_count = 0;
    for (const auto& id : body["speakers"]) {
      if (!id.is_string()) continue;
      nlohmann::json one{{"speaker", id}};
      auto res = sendToSpeaker(one, command, payload);
      const bool good = res.status == 200;
      if (good) ++ok_count;
      results.push_back({{"speaker", id}, {"ok", good}});
    }
    return ok({{"results", results}, {"ok_count", ok_count}, {"total", results.size()}});
  }
  if (p == "/api/volume") {
    if (!body.contains("volume")) return badRequest("volume required");
    return sendToSpeaker(body, nexus::control::cmd::kSetVolume, {{"volume", body["volume"]}});
  }
  if (p == "/api/mute") {
    if (!body.contains("muted")) return badRequest("muted required");
    return sendToSpeaker(body, nexus::control::cmd::kSetMute, {{"muted", body["muted"]}});
  }
  if (p == "/api/eq") {
    if (!body.contains("eq_profile")) return badRequest("eq_profile required");
    return sendToSpeaker(body, nexus::control::cmd::kSetEq, {{"eq_profile", body["eq_profile"]}});
  }
  if (p == "/api/delay") {
    if (!body.contains("delay_ms")) return badRequest("delay_ms required");
    return sendToSpeaker(body, nexus::control::cmd::kSetDelay, {{"delay_ms", body["delay_ms"]}});
  }
  // Per-speaker trim, distinct from /api/volume: volume is the listener's 0..100 control, this is
  // the installer's channel-matching adjustment that holds at every volume setting.
  if (p == "/api/gain") {
    if (!body.contains("gain_db")) return badRequest("gain_db required");
    return sendToSpeaker(body, nexus::control::cmd::kSetGain, {{"gain_db", body["gain_db"]}});
  }
  if (p == "/api/phase") {
    if (!body.contains("phase_invert")) return badRequest("phase_invert required");
    return sendToSpeaker(body, nexus::control::cmd::kSetPhaseInvert,
                         {{"phase_invert", body["phase_invert"]}});
  }
  // Ask one speaker to measure itself acoustically. The streamer only relays and collects — the
  // chirp must be played and recorded on the speaker's own clock, so it cannot be driven from here.
  //
  // The latency budget is passed through rather than defaulted: a distance computed with the wrong
  // RTL is wrong by metres, and silently supplying a plausible default would hide that.
  if (p == "/api/measure") {
    nlohmann::json payload = nlohmann::json::object();
    for (const char* key : {"duration_s", "f0", "f1", "amplitude", "hardware_rtl_ms",
                            "network_rtl_ms", "speed_of_sound", "temperature_c"}) {
      if (body.contains(key)) payload[key] = body[key];
    }
    return sendToSpeaker(body, nexus::control::cmd::kRunMeasurement, payload);
  }
  if (p == "/api/transport") {
    const std::string action = body.value("action", "");
    const char* command = nullptr;
    if (action == "play") command = nexus::control::cmd::kStartAudio;
    else if (action == "pause") command = nexus::control::cmd::kPauseAudio;
    else if (action == "resume") command = nexus::control::cmd::kResumeAudio;
    else if (action == "stop") command = nexus::control::cmd::kStopAudio;
    else return badRequest("action must be play|pause|resume|stop");

    // Drive this streamer's own audio engine as well as telling the speaker. Local audio starts
    // FIRST so packets are already arriving by the time the speaker acts on the command — the
    // speaker's jitter buffer needs a 10-packet prefill before it releases anything, and its UDP
    // receiver listens regardless of START_AUDIO. A local-audio failure is reported and the command
    // is not sent, so the UI never shows "playing" for a stream that never started.
    if (transport_) {
      const std::string id = body.value("speaker", "");
      if (id.empty()) return badRequest("speaker required");
      auto target = registry_.get(id);
      if (!target) return json(404, {{"error", "unknown speaker: " + id}});
      if (auto st = transport_(action, *target); !st.ok()) {
        return json(500, {{"ok", false}, {"message", st.message()}});
      }
    }
    return sendToSpeaker(body, command, {});
  }

  // ── diagnostics ──
  // Room calibration. Distinct from /api/measure (RUN_MEASUREMENT): measurement reports raw
  // acoustic distance, calibration derives and applies the correction. The kiosk already shows a
  // calibration indicator, so without this route that indicator could never be driven from here.
  if (p == "/api/calibrate") {
    nlohmann::json payload = nlohmann::json::object();
    for (const char* key : {"duration_s", "profile"}) {
      if (body.contains(key)) payload[key] = body[key];
    }
    return sendToSpeaker(body, nexus::control::cmd::kRunCalibration, payload);
  }
  if (p == "/api/self-test") {
    return sendToSpeaker(body, nexus::control::cmd::kRunSelfTest, {});
  }
  // Audio test tone. `duration_s`/`frequency` pass through when supplied; the speaker owns the
  // defaults, so omitting them here means "whatever the speaker considers standard" rather than a
  // second, competing default living on the streamer.
  if (p == "/api/audio-test") {
    nlohmann::json payload = nlohmann::json::object();
    for (const char* key : {"duration_s", "frequency", "channel"}) {
      if (body.contains(key)) payload[key] = body[key];
    }
    return sendToSpeaker(body, nexus::control::cmd::kRunAudioTest, payload);
  }

  // ── system / destructive ──
  //
  // These four change or destroy device state, so each requires an explicit
  // {"confirm": true} in the body. The guard is here rather than in the UI because the REST API is
  // reachable by anything on the LAN — a UI-only confirmation protects the one caller that happens
  // to ask nicely and nothing else.
  //
  // FACTORY_RESET is the sharpest: it erases pairing, so the speaker stops answering this streamer
  // entirely and must be re-provisioned. It gets a second, distinct token beyond `confirm` so it
  // cannot be triggered by a caller that merely learned the confirm convention.
  if (p == "/api/reboot" || p == "/api/update" || p == "/api/reset-network" ||
      p == "/api/factory-reset") {
    if (!body.value("confirm", false)) {
      return badRequest("confirm:true required — this command changes device state");
    }
    if (p == "/api/reboot") return sendToSpeaker(body, nexus::control::cmd::kReboot, {});
    if (p == "/api/reset-network") {
      return sendToSpeaker(body, nexus::control::cmd::kResetNetwork, {});
    }
    if (p == "/api/update") {
      nlohmann::json payload = nlohmann::json::object();
      for (const char* key : {"url", "version", "checksum"}) {
        if (body.contains(key)) payload[key] = body[key];
      }
      return sendToSpeaker(body, nexus::control::cmd::kUpdateSoftware, payload);
    }
    // /api/factory-reset
    if (body.value("acknowledge_unpair", "") != "yes") {
      return badRequest(
          "acknowledge_unpair:\"yes\" required — factory reset erases pairing and the speaker "
          "will need to be provisioned again");
    }
    return sendToSpeaker(body, nexus::control::cmd::kFactoryReset, {});
  }
  return notFound();
}

HttpResponse StreamerApiRouter::sendToSpeaker(const nlohmann::json& body, const std::string& command,
                                              const nlohmann::json& payload) const {
  const std::string id = body.value("speaker", "");
  if (id.empty()) return badRequest("speaker required");
  auto target = registry_.get(id);
  if (!target) return json(404, {{"error", "unknown speaker: " + id}});

  // The gateway funnels every reply into the state store before this returns, so by the time we
  // shape the response the store already holds whatever the speaker confirmed — never the value the
  // user asked for.
  auto reply = sender_(*target, command, payload);
  if (!reply.ok()) {
    return json(502, {{"ok", false}, {"message", reply.status().message()}});
  }

  nlohmann::json out{{"ok", reply.value().ok},
                     {"message", reply.value().message},
                     {"data", reply.value().data}};
  // Hand back the confirmed snapshot so the UI can settle on it directly instead of optimistically
  // rendering what it just sent and waiting for the next poll to contradict it.
  if (store_) {
    if (auto st = store_->get(id)) out["confirmed"] = st->toJson(store_->now());
  }
  return ok(out);
}

}  // namespace nexus::streamer::web
