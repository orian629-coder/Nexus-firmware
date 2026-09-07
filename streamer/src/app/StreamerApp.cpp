#include "app/StreamerApp.h"

#include <chrono>
#include <thread>
#include <utility>

#ifdef NEXUS_STREAMER_REAL_NET
#include <httplib.h>
#endif

#include <sodium.h>

#include "audio/AudioPacket.h"
#include "core/StreamerEvents.h"
#include "discovery/ApJoiner.h"
#include "discovery/ApScanner.h"
#include "discovery/NetworkScanner.h"
#include "identity/Crypto.h"

namespace nexus::streamer::app {

using core::ErrorCode;
using core::Status;

namespace {

// Call a speaker's own web API. Returns (http_status, body); status 0 means unreachable.
//
// The speaker's write endpoints need its bearer token, which is derived from the device id
// ("nexus-" + device_id) — the same derivation the speaker itself uses, so the streamer does not
// have to store a per-speaker secret. That token is only as private as the device id, which the
// speaker serves unauthenticated; it is a LAN-trust boundary, not a real secret, and is worth
// tightening on the speaker side later.
std::pair<int, std::string> speakerHttp(const group::Speaker& target, const std::string& method,
                                        const std::string& path, const std::string& payload) {
#ifdef NEXUS_STREAMER_REAL_NET
  httplib::Client cli(target.host, 8080);
  cli.set_connection_timeout(3, 0);
  cli.set_read_timeout(15, 0);  // calibration and self-test are slow

  httplib::Headers headers{{"Authorization", "Bearer nexus-" + target.device_id}};
  httplib::Result res = (method == "POST")
                            ? cli.Post(path.c_str(), headers, payload, "application/json")
                            : cli.Get(path.c_str(), headers);
  if (!res) return {0, ""};
  return {res->status, res->body};
#else
  (void)target; (void)method; (void)path; (void)payload;
  return {0, ""};
#endif
}

}  // namespace

StreamerApp::StreamerApp(const identity::StreamerIdentity& id, control::ILineTransport& line,
                         send::IPacketSink& sink, nexus::web::IWebTransport& web, Options options)
    : id_(id), line_(line), sink_(sink), web_(web), options_(options) {}

StreamerApp::~StreamerApp() { shutdown(); }

Status StreamerApp::startup() {
  // Config first: the registry, zones, and the web token are all hydrated from it, and everything
  // below depends on those being populated before it starts.
  if (!options_.config_path.empty()) {
    config_ = std::make_unique<config::StreamerConfigManager>(options_.config_path);
    // A corrupt config is Degraded, not fatal — the streamer still runs with defaults, which beats
    // refusing to boot and leaving the user with no UI to fix it from.
    config_->start();

    const auto cfg = config_->get();
    for (const auto& s : cfg.speakers) {
      registry_.upsert({s.device_id, s.name, s.host, s.control_port});
    }
    auth_token_ = cfg.web.auth_token;
  }

  // Generate the API token on first run and persist it, so the browser stays authorized across
  // restarts instead of demanding a new token every boot.
  if (auth_token_.empty()) {
    unsigned char raw[24];
    randombytes_buf(raw, sizeof(raw));
    auth_token_ = nexus::identity::crypto::toBase64(
        std::vector<std::uint8_t>(raw, raw + sizeof(raw)));
    if (config_) {
      config_->update([this](config::StreamerConfig& c) { c.web.auth_token = auth_token_; });
    }
  }
  auth_ = std::make_unique<nexus::web::Authentication>(auth_token_);

  zones_ = std::make_unique<group::ZoneManager>(registry_, &store_);
  if (config_) {
    std::vector<group::Zone> loaded;
    for (const auto& z : config_->get().zones) loaded.push_back({z.zone_id, z.name, z.members});
    zones_->replaceAll(std::move(loaded));
  }

  gateway_ = std::make_unique<CommandGateway>(line_, id_.secretKeyBase64(), id_.streamerId());

  // THE source-of-truth rule, enforced in one place: every exchange with a speaker — a user's
  // SET_VOLUME as much as a background poll — lands in the store here, and only ever carries what
  // the speaker itself replied. Nothing else may write confirmed state.
  //
  // A successful set self-confirms with no extra round trip: SET_VOLUME/SET_MUTE/SET_DELAY/SET_EQ
  // all echo the applied value in `data`. The speaker's nine deferred commands echo only
  // {"accepted":true,"deferred":true}, which carries no state keys, so an ack from a command that
  // did nothing confirms nothing.
  gateway_->setReplyObserver([this](const std::string& device_id, const core::Status& status,
                                    const control::CommandReply& reply) {
    if (!status.ok()) {
      store_.applyPollFailure(device_id, status.message(), options_.offline_threshold);
    } else if (reply.ok) {
      store_.applyConfirmedStatus(device_id, reply.data);
    } else {
      store_.noteRejected(device_id, reply.message);
    }
  });

  audio_ = std::make_unique<audio::AudioEngine>(sink_, &bus_, options_.sample_rate,
                                                options_.channels, options_.block_frames,
                                                options_.lead_seconds);

  // Master DSP sits between the source and the packetizer, so every speaker gets the same processed
  // audio. Restored from config before the hook is installed: a saved EQ curve must be in effect on
  // the very first block, not applied a moment later once the UI happens to push it.
  master_dsp_ = std::make_unique<streamer::dsp::MasterDsp>(
      static_cast<double>(options_.sample_rate), options_.channels);
  if (config_) {
    master_dsp_->setConfig(streamer::dsp::MasterDspConfig::fromJson(config_->get().dsp));
  }
  // Raw pointer capture is safe: master_dsp_ is declared before audio_ and so outlives the engine's
  // thread, which is joined in the engine's destructor.
  auto* dsp_ptr = master_dsp_.get();
  audio_->setDspHook([dsp_ptr](std::int16_t* io, std::size_t frames, int channels) {
    dsp_ptr->process(io, frames, channels);
  });

  ordered_.push_back(audio_.get());

  if (options_.enable_link_reporter) {
    link_ = std::make_unique<LinkReporterService>(*gateway_, registry_, id_.streamerId(),
                                                  options_.link_interval_ms);
    ordered_.push_back(link_.get());
  }

  monitor_ = std::make_unique<state::MonitorService>(*gateway_, registry_, store_,
                                                     options_.poll_interval_ms);
  if (options_.enable_monitor) ordered_.push_back(monitor_.get());

  // Advertise _nexus-streamer._tcp so speakers can find us (no-op without Avahi).
  discovery_ = std::make_unique<discovery::DiscoveryService>(
      options_.discovery, id_.streamerId(), id_.publicKeyBase64(), options_.web_port);
  ordered_.push_back(discovery_.get());

  for (auto* svc : ordered_) {
    if (auto st = svc->start(); !st.ok()) {
      shutdown();
      return Status::error(st.code(), svc->name() + ": " + st.message());
    }
    started_.push_back(svc);
  }

  // The web server goes up last: the moment it listens, a request can arrive and touch the audio
  // engine and the gateway, so both must already be running.
  router_ = std::make_unique<web::StreamerApiRouter>(
      registry_,
      [this](const group::Speaker& target, const std::string& command,
             const nlohmann::json& payload) { return gateway_->send(target, command, payload); },
      // Pairing: the UI supplies the speaker-specific parts (host, setup code, box key, Wi-Fi
      // creds); the streamer's own identity is filled in here because the browser must never see
      // the secret key. Wiring this was missed when StreamerApp replaced the old serve(), which
      // left /api/pair returning 501 in the real binary even though the client and its tests were
      // fine — a speaker could not be paired from the shipped UI at all.
      [this](const std::string& host, pairing::PairingParams params) {
        params.streamer_id = id_.streamerId();
        params.streamer_public_key = id_.publicKeyBase64();
        params.streamer_secret_key = id_.secretKeyBase64();
        pairing::PairingClient client(line_, host, /*control_port=*/45455);
        return client.pair(params);
      },
      /*discoverer=*/nullptr,
      // Network scan: sweep the local /24 and report every host that answers like a Nexus speaker.
      // This was passed as nullptr, so "Scan Network" returned 501 in the shipped binary even
      // though NetworkScanner and its tests were fine — the same class of wiring gap that left
      // /api/pair dead until it was noticed.
      [] {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : discovery::scanSubnet(discovery::localSubnetOr("192.168.1.0/24"))) {
          arr.push_back({{"ip", s.ip},
                         {"device_id", s.device_id},
                         {"state", s.state},
                         {"software_version", s.software_version},
                         {"paired", s.paired},
                         {"setup_mode", s.setup_mode},
                         {"box_public_key", s.box_public_key}});
        }
        return arr;
      },
      [this](const std::string& action, const group::Speaker& target) {
        return onTransport(action, target);
      },
      &store_, auth_.get(),
      // Setup-AP scan: a speaker that has never held credentials answers neither mDNS nor the
      // subnet sweep, so the only way to see it is the AP it broadcasts.
      [] {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& ap : discovery::scanAccessPoints()) {
          arr.push_back({{"ssid", ap.ssid},
                         {"bssid", ap.bssid},
                         {"signal", ap.signal},
                         {"secured", ap.secured}});
        }
        return arr;
      },
      // Onboarding over a setup AP. The radio is parked on the speaker's AP only for the duration
      // of the handshake and released again on every path out, including failure — a stranded
      // profile would leave the streamer unable to scan for the next speaker.
      [this](const nlohmann::json& body) -> nlohmann::json {
        discovery::ApJoiner joiner;
        const std::string ssid = body.value("ssid", "");
        if (auto st = joiner.join(ssid); !st.ok()) {
          return {{"ok", false}, {"message", st.message()}};
        }
        // The speaker is the gateway of the network it is hosting. Ask for the real address rather
        // than assuming the documented one, so a changed hotspot subnet fails loudly here instead
        // of timing out later against a host that was never there.
        const std::string host = joiner.gateway();
        if (host.empty()) {
          joiner.leave();
          return {{"ok", false}, {"message", "joined the setup network but found no speaker on it"}};
        }
        pairing::PairingParams params;
        params.streamer_id = id_.streamerId();
        params.streamer_public_key = id_.publicKeyBase64();
        params.streamer_secret_key = id_.secretKeyBase64();
        params.site_id = body.value("site_id", "default");
        params.initial_speaker_name = body.value("name", ssid);
        params.setup_code = body.value("setup_code", "");
        params.wifi_ssid = body.value("wifi_ssid", "");
        params.wifi_psk = body.value("wifi_psk", "");
        params.speaker_box_public_key = body.value("box_public_key", "");
        pairing::PairingClient client(line_, host, /*control_port=*/45455);
        auto reply = client.pair(params);
        joiner.leave();  // release the radio before reporting, success or not

        if (!reply.ok()) return {{"ok", false}, {"message", reply.status().message()}};
        if (!reply.value().ok) return {{"ok", false}, {"message", reply.value().message}};

        // Paired. The speaker is now joining the real Wi-Fi and will get a new address there, so
        // its host is left empty deliberately: the next discovery/scan fills it in. Recording the
        // AP-side gateway would persist an address that stops existing the moment the AP goes down.
        group::Speaker s;
        s.device_id = reply.value().device_id;
        s.name = body.value("name", s.device_id);
        s.host = "";
        s.control_port = 45455;
        registry_.upsert(s);
        persist();
        return {{"ok", true},
                {"device_id", s.device_id},
                {"message", "paired; the speaker is joining the network"}};
      },
      &provisioning_window_);
  router_->setSpeakerHttp([](const group::Speaker& target, const std::string& method,
                             const std::string& path, const std::string& payload) {
    return speakerHttp(target, method, path, payload);
  });
  router_->setMacLookup([](const std::string& ip) { return discovery::macForIp(ip); });
  router_->setZones(zones_.get());
  router_->setPlayZone([this](const std::string& zone_id, const sources::SourceSpec& spec) {
    return playZone(zone_id, spec);
  });
  router_->setRetargetZone([this](const std::string& zone_id) { return retargetZone(zone_id); });
  router_->setPersist([this] { persist(); });

  // Live signal levels. Both meters are lock-free, so this handler never blocks the audio thread
  // no matter how fast the browser polls it.
  router_->setLevels([this] {
    const auto in = audio_->inputMeter().read();
    const auto out = audio_->outputMeter().read();
    const auto t = audio_->transport();
    auto pack = [](const nexus::streamer::audio::LevelMeter::Levels& v) {
      const double db_l = nexus::streamer::audio::toDbfs(v.rms_left);
      const double db_r = nexus::streamer::audio::toDbfs(v.rms_right);
      return nlohmann::json{
          {"rms_db_left", db_l},
          {"rms_db_right", db_r},
          // Bar fractions are computed here so the browser and the C++ side cannot disagree about
          // where the bottom of the scale sits.
          {"bar_left", nexus::streamer::audio::dbToFraction(db_l)},
          {"bar_right", nexus::streamer::audio::dbToFraction(db_r)},
          {"peak_db_left", nexus::streamer::audio::toDbfs(v.peak_left)},
          {"peak_db_right", nexus::streamer::audio::toDbfs(v.peak_right)},
          // Only true digital full-scale counts as clipping; anything lower is just loud.
          {"clipping", v.peak_left >= 0.999f || v.peak_right >= 0.999f},
          {"active", v.active}};
    };
    const char* state = t.state == audio::AudioEngine::State::Playing   ? "playing"
                        : t.state == audio::AudioEngine::State::Paused  ? "paused"
                                                                        : "idle";
    return nlohmann::json{{"input", pack(in)},
                          {"output", pack(out)},
                          {"state", state},
                          {"packets_sent", t.packets_sent},
                          {"targets", t.targets}};
  });

  // Master DSP seams. The setter MERGES onto the config currently in effect rather than replacing
  // it, so moving one slider cannot silently reset the other 31 EQ bands — the UI sends only what
  // changed. It returns the config actually applied (after clamping), which is what the UI displays.
  router_->setDspAccess(
      [this] { return master_dsp_->config().toJson(); },
      [this](const nlohmann::json& patch) {
        nlohmann::json merged = master_dsp_->config().toJson();
        if (patch.is_object()) merged.update(patch);
        master_dsp_->setConfig(streamer::dsp::MasterDspConfig::fromJson(merged));
        return master_dsp_->config().toJson();
      });

  if (auto st = web_.start(options_.web_port,
                           [this](const nexus::web::HttpRequest& req) {
                             return router_->route(req);
                           });
      !st.ok()) {
    shutdown();
    return Status::error(st.code(), "web: " + st.message());
  }
  web_started_ = true;
  return Status::success();
}

Status StreamerApp::shutdown() {
  if (web_started_) {
    web_.stop();
    web_started_ = false;
  }
  // Reverse order, so nothing is torn down while something above it can still call into it.
  for (auto it = started_.rbegin(); it != started_.rend(); ++it) {
    (*it)->stop();
  }
  started_.clear();
  ordered_.clear();
  return Status::success();
}

Status StreamerApp::playTo(const std::vector<audio::Endpoint>& targets,
                           std::unique_ptr<sources::IAudioSource> source) {
  if (!audio_) return Status::error(ErrorCode::InvalidArg, "audio engine not started");

  std::vector<std::pair<std::string, int>> t;
  t.reserve(targets.size());
  for (const auto& e : targets) t.emplace_back(e.host, e.port);
  if (auto st = sink_.setTargets(t); !st.ok()) return st;
  audio_->setTargetCount(t.size());

  return audio_->play(std::move(source));
}

// The UI's transport buttons drive this streamer's own audio engine. Phase 1 covers pause/resume/
// stop and the retarget half of play; selecting *what* to play (a file, a capture device) arrives
// with the real sources in Phase 5, so a play with no source loaded only points the fan-out at the
// speaker and leaves the engine idle rather than failing the request.
Status StreamerApp::onTransport(const std::string& action, const group::Speaker& target) {
  if (!audio_) return Status::error(ErrorCode::InvalidArg, "audio engine not started");

  if (action == "play") {
    auto st = sink_.setTargets({{target.host, nexus::audio::wire::kDefaultPort}});
    if (st.ok()) audio_->setTargetCount(1);
    return st;
  }
  if (action == "pause") {
    audio_->pause();
    return Status::success();
  }
  if (action == "resume") {
    audio_->resume();
    return Status::success();
  }
  if (action == "stop") {
    audio_->stopStream();
    return Status::success();
  }
  return Status::error(ErrorCode::InvalidArg, "unknown transport action: " + action);
}

Status StreamerApp::playZone(const std::string& zone_id, const sources::SourceSpec& spec) {
  if (!zones_) return Status::error(ErrorCode::InvalidArg, "zones not available");

  auto targets = zones_->resolveTargets(zone_id);
  if (!targets.ok()) return targets.status();
  if (targets.value().empty()) {
    return Status::error(ErrorCode::NotFound, "zone has no reachable speakers");
  }

  auto source = sources::makeSource(spec);
  if (!source.ok()) return source.status();

  std::vector<audio::Endpoint> endpoints;
  endpoints.reserve(targets.value().size());
  for (const auto& t : targets.value()) endpoints.push_back({t.host, t.port});

  // Tell every member to start before the audio arrives, then begin sending.
  for (const auto& sp : zones_->members(zone_id)) {
    gateway_->send(sp, "START_AUDIO", nlohmann::json::object());
  }
  return playTo(endpoints, std::move(source.value()));
}

Status StreamerApp::retargetZone(const std::string& zone_id) {
  if (!zones_) return Status::error(ErrorCode::InvalidArg, "zones not available");
  if (!audio_) return Status::error(ErrorCode::InvalidArg, "audio engine not started");

  auto targets = zones_->resolveTargets(zone_id);
  if (!targets.ok()) return targets.status();

  std::vector<std::pair<std::string, int>> t;
  t.reserve(targets.value().size());
  for (const auto& e : targets.value()) t.emplace_back(e.host, e.port);
  if (auto st = sink_.setTargets(t); !st.ok()) return st;
  audio_->setTargetCount(t.size());

  // Newly added members have not been told to expect audio, and one that was dropped keeps its
  // amplifier live until told otherwise. START_AUDIO is idempotent, so sending it to the current
  // set is enough.
  for (const auto& sp : zones_->members(zone_id)) {
    gateway_->send(sp, "START_AUDIO", nlohmann::json::object());
  }
  return Status::success();
}

void StreamerApp::persist() {
  if (!config_) return;
  config_->update([this](config::StreamerConfig& c) {
    c.speakers.clear();
    for (const auto& s : registry_.list()) {
      config::SpeakerRecord r;
      r.device_id = s.device_id;
      r.name = s.name;
      r.host = s.host;
      r.control_port = s.control_port;
      c.speakers.push_back(std::move(r));
    }
    c.zones.clear();
    if (zones_) {
      for (const auto& z : zones_->list()) c.zones.push_back({z.zone_id, z.name, z.members});
    }
    // The master chain is user-set configuration (unlike speaker state, which is mirrored and
    // deliberately ephemeral), so a tuned EQ curve survives a restart.
    if (master_dsp_) c.dsp = master_dsp_->config().toJson();
  });
}

int StreamerApp::run() {
  running_ = true;
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  shutdown();
  return 0;
}

}  // namespace nexus::streamer::app
