#include "main/Application.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <mutex>

#include "logging/Logger.h"

// Stub module facades, in startup order.
#include "network/NetworkManager.h"
#include "network/StreamerApJoin.h"
#include "discovery/DiscoveryService.h"
#include "pairing/PairingService.h"
#include "pairing/ProvisioningController.h"
#include "control/Command.h"  // control::cmd:: name constants
#include "control/CommandServer.h"
#include "audio/AudioReceiver.h"
#include "dsp/DspEngine.h"
#include "amplifier/AmplifierManager.h"
#include "measure/MeasurementService.h"
#include "microphone/MicrophoneManager.h"
#include "calibration/CalibrationManager.h"
#include "status/StatusService.h"
#include "diagnostics/DiagnosticService.h"
#include "updater/UpdateManager.h"
#include "storage/LocalStorage.h"
#include "web/WebServer.h"

namespace nexus {

using core::ErrorCode;
using core::IService;
using core::ServiceState;
using core::Status;

namespace {
std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;
}  // namespace

Application::Application(AppOptions opts) : opts_(std::move(opts)) {}

Application::~Application() { shutdown(); }

void Application::buildServices() {
  // Fully-implemented services (Phase 1 + Phase 2).
  secure_ = std::make_unique<storage::SecureStorage>(opts_.secure_dir);
  keys_ = std::make_unique<identity::KeyManager>(secure_.get());
  config_ = std::make_unique<config::ConfigManager>(opts_.config_path, &bus_);
  identity_ = std::make_unique<identity::DeviceIdentity>(opts_.identity_path, secure_.get(), &bus_);
  system_ = std::make_unique<system::SystemManager>(&bus_);
  network_ = std::make_unique<network::NetworkManager>(&bus_);
  discovery_ = std::make_unique<discovery::DiscoveryService>(&bus_);
  pairing_ =
      std::make_unique<pairing::PairingService>(&bus_, keys_.get(), config_.get(), secure_.get());
  // BLE provisioning channel: while the speaker has no network it advertises a BLE setup service
  // (Just Works, no code) so a phone/streamer app can hand over Wi-Fi + streamer credentials; once
  // online it stops, and it comes back if the network later drops. The BLE GATT server runs as a
  // separate helper (nexus-provisioning.service); this controller just gates it on connectivity.
  // Probe live connectivity at the controller's start() (network_ starts earlier in ordered_, so
  // its state is accurate by then). This closes the boot race where the one-shot NetworkConnected
  // fires before the controller subscribes — without it a wired speaker leaves the hotspot up and
  // deadlocks wlan0.
  auto* net = network_.get();
  provisioning_ = std::make_unique<pairing::ProvisioningController>(
      &bus_, /*actuator=*/nullptr, [net]() -> pairing::ProvisioningController::InitialState {
        return {/*have_uplink=*/net->isConnected(), /*on_wifi=*/net->mode() == "wifi"};
      });
  // Wall-clock provider (epoch seconds) shared by the command server and status service.
  auto clock = [] { return static_cast<std::int64_t>(::time(nullptr)); };
  control_ = std::make_unique<control::CommandServer>(&bus_, identity_.get(), config_.get(), clock);
  status_ = std::make_unique<status::StatusService>(&bus_, identity_.get(), config_.get(), clock);
  // Audio receiver uses a high-resolution epoch-seconds clock for stream sync.
  auto audio_clock = [] {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  audio_ = std::make_unique<audio::AudioReceiver>(&bus_, &audio_buffer_, &audio_sync_, audio_clock);
  dsp_ = std::make_unique<dsp::DspEngine>(&bus_);
  // Playback engine: pulls from the jitter buffer, runs the DSP chain, writes to the output HAL.
  playback_ = std::make_unique<audio::PlaybackEngine>(&bus_, &audio_buffer_, &audio_sync_,
                                                      config_.get(), audio_clock);
  playback_->setDspHook(
      [this](std::int16_t* io, std::size_t frames, int ch) { dsp_->processInt16(io, frames, ch); });
  amplifier_ = std::make_unique<amplifier::AmplifierManager>(&bus_);
  microphone_ = std::make_unique<microphone::MicrophoneManager>(&bus_);
  diagnostics_ = std::make_unique<diagnostics::DiagnosticService>(&bus_);
  cal_profiles_ = std::make_shared<calibration::CalibrationProfiles>(opts_.calibration_dir);
  calibration_ = std::make_unique<calibration::CalibrationManager>(&bus_, config_.get(),
                                                                   cal_profiles_);
  // Wire calibration to the real hardware/DSP: capture from the mic, apply EQ to the DSP, mute via
  // the amplifier around the measurement.
  calibration_->setHooks(
      [this](std::size_t frames) {
        auto pcm = microphone_->capture(frames);
        return pcm.ok() ? pcm.value() : std::vector<std::int16_t>{};
      },
      [this](const std::array<double, dsp::kEqBands>& gains) { dsp_->setEqGains(gains); },
      [this](bool muted) { muted ? amplifier_->mute() : amplifier_->unmute(); });

  // Local web interface + API. The maintenance token is derived from the device id (a real
  // deployment would provision a stronger secret); read endpoints are open on the LAN, writes
  // require the token. The port is overridable via NEXUS_WEB_PORT (default 8080) so it can move
  // if another local service already owns 8080.
  const std::string web_token = "nexus-" + identity_->deviceId();
  int web_port = web::WebServer::kDefaultPort;
  if (const char* p = std::getenv("NEXUS_WEB_PORT")) {
    try {
      web_port = std::stoi(p);
    } catch (...) { /* keep default */ }
  }
  web_ = std::make_unique<web::WebServer>(&bus_, buildApiContext(), web_token, nullptr, web_port);

  // Playback health inside GET_STATUS, so one poll tells the streamer not just that this speaker is
  // reachable but whether it is actually playing cleanly. Counters are cumulative since boot; the
  // streamer derives rates by differencing consecutive polls, which is what makes "losing packets
  // right now" distinguishable from "lost some packets an hour ago".
  control_->setTelemetryProvider([this] {
    const auto m = audio_buffer_.metrics();
    const auto s = audio_sync_.stats();
    // received+lost is the total the sender emitted as far as this speaker can tell, which is the
    // only honest denominator for a loss percentage.
    const std::uint64_t expected = m.received + m.lost;
    const double loss_pct =
        expected > 0 ? (100.0 * static_cast<double>(m.lost) / static_cast<double>(expected)) : 0.0;
    return nlohmann::json{
        {"buffer_depth", m.depth},
        {"packets_received", m.received},
        {"packets_lost", m.lost},
        {"packet_loss_pct", loss_pct},
        // Underflow = the buffer ran dry and playback had nothing to emit; overflow = packets
        // arrived faster than they were consumed and had to be dropped. Both are audible as
        // dropouts, so they are reported separately from plain loss.
        {"underflows", m.underflows},
        {"dropped_overflow", m.dropped_overflow},
        {"latency_ms", s.latency_ms},
        {"clock_offset_ms", s.clock_offset_ms},
        {"drift_ppm", s.drift_ppm}};
  });

  // Acoustic measurement (RUN_MEASUREMENT): emit a chirp from this speaker and report what its
  // microphones heard. Wired here because Application is the only place that owns both the output
  // and the microphone.
  //
  // LIMITATION, stated in the reply rather than hidden: the current IMicrophoneHal captures mono and
  // exposes no way to start playback and capture from the same clock. The measured lag therefore
  // includes an unknown scheduling gap between "start playing" and "start recording", which is
  // exactly the kind of error that produces confident nonsense — a 10 ms gap is 3.4 m. So the
  // measurement runs and reports its numbers, but marks itself `synchronized:false` so the caller
  // can refuse to build geometry on it. Closing this needs a HAL that does a single duplex
  // transfer (the prototype used sounddevice's playrec, which does exactly that).
  control_->setMeasurementRunner([this](const nlohmann::json& params) -> nlohmann::json {
    measure::MeasurementRequest req;
    req.sample_rate = 48000.0;
    req.duration_s = params.value("duration_s", 1.0);
    req.f0 = params.value("f0", 1000.0);
    req.f1 = params.value("f1", 15000.0);
    req.amplitude = params.value("amplitude", 0.4);
    req.budget.hardware_rtl_ms = params.value("hardware_rtl_ms", 0.0);
    req.budget.network_rtl_ms = params.value("network_rtl_ms", 0.0);
    if (params.contains("speed_of_sound")) {
      req.speed_of_sound = params["speed_of_sound"].get<double>();
    } else if (params.contains("temperature_c")) {
      req.speed_of_sound = measure::speedOfSoundAt(params["temperature_c"].get<double>());
    }

    measure::MeasurementService svc(
        [this](const std::vector<double>& stimulus) -> measure::MeasurementService::Capture {
          measure::MeasurementService::Capture cap;
          // Capture a window longer than the stimulus so a late arrival still lands inside it.
          const std::size_t frames = stimulus.size() * 2;
          auto pcm = microphone_->capture(frames);
          if (!pcm.ok()) return cap;
          cap.pcm = std::move(pcm.value());
          cap.channels = 1;  // see the limitation above
          return cap;
        });

    const auto r = svc.measure(req);

    nlohmann::json mics = nlohmann::json::array();
    for (const auto& m : r.mics) {
      nlohmann::json j{{"mic", m.mic_index + 1},  // 1-based for the UI, matching "Mic1"/"Mic2"
                       {"valid", m.valid},
                       {"lag_ms", m.lag_ms},
                       {"confidence", m.confidence}};
      if (m.valid) j["distance_m"] = m.distance_m;
      if (!m.note.empty()) j["note"] = m.note;
      mics.push_back(std::move(j));
    }
    return nlohmann::json{{"ok", r.ok},
                          {"message", r.message},
                          {"mics", std::move(mics)},
                          {"synchronized", false},
                          {"limitation", "capture is not clock-locked to playback on this build; "
                                         "distances carry an unknown scheduling offset"}};
  });

  // Route inbound pairing requests (JSON type=="pairing" on the command port) to the pairing
  // service. The request carries the streamer's Ed25519 key, a setup code, a signature over the
  // canonical bytes, and the sealed Wi-Fi credentials; PairingService validates and applies it.
  control_->setPairingHandler([this](const std::string& raw) -> std::string {
    pairing::PairingRequest req;
    try {
      auto j = nlohmann::json::parse(raw);
      req.streamer_id = j.value("streamer_id", "");
      req.streamer_public_key = j.value("streamer_public_key", "");
      req.site_id = j.value("site_id", "");
      req.initial_speaker_name = j.value("initial_speaker_name", "");
      req.setup_code = j.value("setup_code", "");
      req.sealed_wifi_b64 = j.value("sealed_wifi", "");
      req.signature_b64 = j.value("signature", "");
    } catch (const std::exception& e) {
      return nlohmann::json{{"type", "pairing_result"}, {"ok", false},
                            {"message", std::string("bad pairing json: ") + e.what()}}
          .dump();
    }
    auto now = static_cast<std::int64_t>(::time(nullptr));
    auto res = pairing_->processRequest(req, now);
    if (!res.ok()) {
      return nlohmann::json{{"type", "pairing_result"}, {"ok", false},
                            {"message", res.status().message()}}
          .dump();
    }
    // Pairing succeeded. Join the Wi-Fi it delivered ONLY if we have no uplink yet.
    //
    // Pairing always carries Wi-Fi credentials because the common case is onboarding a speaker that
    // has no network. But applying them unconditionally means pairing an ALREADY-CONNECTED speaker
    // tears down its working connection and re-joins from scratch — and if those credentials are
    // wrong or stale, the speaker drops off the network entirely and has to be recovered through the
    // setup hotspot. That happened on a live device: pairing a speaker that was happily on Wi-Fi
    // knocked it off the LAN.
    //
    // A speaker that already has an uplink does not need the credentials, so honour the pairing and
    // leave the working connection alone. Onboarding (no uplink) is unaffected.
    const bool have_uplink = network_ && network_->isConnected();
    if (have_uplink) {
      NX_LOG_INFO("pairing", "paired; keeping the existing network connection (already online)");
    } else if (res.value().wifi.ssid.empty()) {
      NX_LOG_WARN("pairing", "paired but no wifi credentials supplied and no uplink present");
    } else {
      network_->connectWifi(res.value().wifi.ssid, res.value().wifi.psk);
    }
    return nlohmann::json{{"type", "pairing_result"}, {"ok", true},
                          {"device_id", identity_->deviceId()}}
        .dump();
  });

  // Signed OTA updater. Guard updates during calibration.
  updater_ = std::make_unique<updater::UpdateManager>(&bus_, opts_.vendor_public_key_b64,
                                                      opts_.binary_path);

  // Watchdog with recovery escalation. A service crossing the failure threshold degrades the
  // system; repeated failures escalate to Safe Mode via SystemManager.
  watchdog_ = std::make_unique<system::Watchdog>(&bus_);
  watchdog_->setRecoveryHandler([this](const std::string& service) {
    NX_LOG_WARN("main", "watchdog recovery for '" + service + "'");
    // Escalation: the WatchdogTimeout event (published by the watchdog) already reaches
    // SystemManager, which degrades and, on repeated failures, enters Safe Mode.
  });

  // Safe Mode disables normal operation: mute the amp and stop audio, while web/logs stay up.
  system_->setSafeModeDisableAction([this] {
    amplifier_->mute();
    audio_->stop();
  });

  // Refuse OTA updates while a calibration is in progress (spec: no update during calibration).
  bus_.subscribe(core::EventType::CalibrationStarted,
                 [this](const core::Event&) { updater_->setUpdatesAllowed(false); });
  bus_.subscribe(core::EventType::CalibrationCompleted,
                 [this](const core::Event&) { updater_->setUpdatesAllowed(true); });
  bus_.subscribe(core::EventType::CalibrationFailed,
                 [this](const core::Event&) { updater_->setUpdatesAllowed(true); });

  // Amplifier gating around playback: unmute when audio starts (once the amp is stable), mute when
  // audio stops — keeps the driver silent unless there's a real stream.
  bus_.subscribe(core::EventType::AudioStarted, [this](const core::Event&) {
    if (!config_->get().audio.muted) amplifier_->unmute();
  });
  bus_.subscribe(core::EventType::AudioStopped,
                 [this](const core::Event&) { amplifier_->mute(); });

  // Drive streamer discovery off the state machine. Entering SEARCHING_STREAMER means we are paired
  // and online but not yet talking to our streamer, so kick off a continuous mDNS browse for the
  // paired streamer_id (verified against its stored public key). Leaving the state stops the worker.
  //
  // This is the wiring that was missing: DiscoveryService::findStreamer() had no caller, so a paired
  // speaker reached SEARCHING_STREAMER and sat there forever. StateChanged carries from/to as the
  // StateMachine's string names (see system::toString).
  bus_.subscribe(core::EventType::StateChanged, [this](const core::Event& e) {
    const std::string to = e.data.value("to", "");
    const std::string from = e.data.value("from", "");
    if (to == system::toString(system::SystemState::SearchingStreamer)) {
      const auto& p = config_->get().pairing;
      if (p.streamer_id.empty()) {
        NX_LOG_WARN("main", "entered SEARCHING_STREAMER with no paired streamer_id — cannot search");
        return;
      }
      discovery_->startSearching(p.streamer_id, p.streamer_public_key);
    } else if (from == system::toString(system::SystemState::SearchingStreamer)) {
      discovery_->stopSearching();
    }
  });

  // On entering CONNECTING_NETWORK, a paired speaker best-effort joins its streamer's private AP
  // ("Nexus-<streamer_id>"). NotFound (AP not in range) is normal — the usual network path
  // (persisted profile / ethernet) still applies. See docs/STREAMER-AP-DESIGN.md.
  bus_.subscribe(core::EventType::StateChanged, [this](const core::Event& e) {
    if (e.data.value("to", "") != system::toString(system::SystemState::ConnectingNetwork)) return;
    const auto& p = config_->get().pairing;
    if (!p.paired || p.streamer_id.empty()) return;
    const core::Status st = network::joinStreamerAp(*network_, p.streamer_id);
    if (!st.ok() && st.code() != core::ErrorCode::NotFound) {
      NX_LOG_WARN("main", "streamer-AP join failed");
    }
  });

  // Push audio settings into the live DSP whenever a signed command changes them.
  //
  // Without this the command path only WROTE CONFIG: SET_DELAY from the streamer was persisted and
  // acknowledged, but the running DSP kept its old value until the next reboot, so the speaker
  // reported a delay it was not actually applying. The web path happened to call the DSP inline,
  // which is why it worked there and not here. Driving it off CommandExecuted keeps the DSP in
  // step with config for every future setting too, instead of relying on each call site to
  // remember.
  bus_.subscribe(core::EventType::CommandExecuted, [this](const core::Event& e) {
    const auto& d = e.data;
    if (d.contains("delay_ms") && d["delay_ms"].is_number_integer()) {
      dsp_->setDelayMs(d["delay_ms"].get<int>());
    }
    if (d.contains("gain_db") && d["gain_db"].is_number()) {
      dsp_->setOutputGainDb(d["gain_db"].get<double>());
    }
    if (d.contains("phase_invert") && d["phase_invert"].is_boolean()) {
      dsp_->setPhaseInvert(d["phase_invert"].get<bool>());
    }
    // RUN_AUDIO_TEST is acknowledged by CommandExecutor as "deferred" — it emits the intent and
    // expects the owning module to act. Nothing did, so the identify tone the installer relies on
    // never played when triggered from the streamer (it worked only via the speaker's own web API).
    if (d.value("command", "") == control::cmd::kRunAudioTest) playIdentifyTone();
  });

  // Register diagnostic checks that query the real modules (kept as probes so diagnostics doesn't
  // hard-depend on them).
  using diagnostics::CheckResult;
  diagnostics_->addCheck("amplifier", [this]() -> std::pair<CheckResult, std::string> {
    auto st = amplifier_->ampState();
    if (st == amplifier::AmpState::Fault) return {CheckResult::Error, "fault"};
    if (st == amplifier::AmpState::Overheated || st == amplifier::AmpState::Protection)
      return {CheckResult::Warning, amplifier::toString(st)};
    return {CheckResult::Ok, ""};
  });
  diagnostics_->addCheck("microphone", [this]() -> std::pair<CheckResult, std::string> {
    return microphone_->selfTest().ok() ? std::make_pair(CheckResult::Ok, std::string())
                                        : std::make_pair(CheckResult::Warning, "mic self-test");
  });
  diagnostics_->addCheck("network", [this]() -> std::pair<CheckResult, std::string> {
    return network_->isConnected() ? std::make_pair(CheckResult::Ok, std::string())
                                   : std::make_pair(CheckResult::Warning, "offline");
  });
  diagnostics_->addCheck("config", [this]() -> std::pair<CheckResult, std::string> {
    return config_->state() == core::ServiceState::Running
               ? std::make_pair(CheckResult::Ok, std::string())
               : std::make_pair(CheckResult::Warning, "config degraded");
  });

  // Helper to create + own a stub service and append to the ordered list.
  auto add_stub = [this](std::unique_ptr<IService> svc) {
    ordered_.push_back(svc.get());
    owned_stubs_.push_back(std::move(svc));
  };

  // Startup order per spec:
  // Logger (already up) → Config → Identity → Storage → Hardware → Network → Discovery →
  // Control → Audio → Status → Web → Watchdog. System manager sits alongside config/identity.
  ordered_.push_back(config_.get());
  ordered_.push_back(identity_.get());
  ordered_.push_back(system_.get());
  add_stub(std::make_unique<storage::LocalStorage>(&bus_));
  ordered_.push_back(amplifier_.get());
  ordered_.push_back(microphone_.get());
  ordered_.push_back(network_.get());
  ordered_.push_back(discovery_.get());
  ordered_.push_back(pairing_.get());
  ordered_.push_back(provisioning_.get());
  ordered_.push_back(control_.get());
  ordered_.push_back(dsp_.get());
  ordered_.push_back(audio_.get());
  ordered_.push_back(playback_.get());
  ordered_.push_back(calibration_.get());
  ordered_.push_back(status_.get());
  ordered_.push_back(diagnostics_.get());
  ordered_.push_back(updater_.get());
  ordered_.push_back(web_.get());
  ordered_.push_back(watchdog_.get());

  for (auto* svc : ordered_) watchdog_->watch(svc);
}

web::ApiContext Application::buildApiContext() {
  web::ApiContext ctx;
  ctx.status = [this] {
    const auto& c = config_->get();
    nlohmann::json j{{"device_id", identity_->deviceId()},
                     {"state", system::toString(system_->states().current())},
                     {"software_version", c.device.software_version},
                     {"volume", c.audio.volume},
                     {"muted", c.audio.muted},
                     {"eq_profile", c.audio.eq_profile},
                     {"paired", c.pairing.paired}};
    // While in setup mode, expose the public X25519 key a Streamer needs to seal Wi-Fi credentials
    // (public key — safe to publish; this mirrors the mDNS TXT record).
    if (pairing_->inSetupMode()) {
      j["setup_mode"] = true;
      j["box_public_key"] = keys_->boxPublicKeyBase64().value_or("");
    }
    // Once paired, expose which streamer we're bound to (public id — safe) so status UIs (kiosk /
    // web) can show the actual source instead of inferring it from raw network link state.
    if (c.pairing.paired) {
      j["streamer_id"] = c.pairing.streamer_id;
    }
    return j;
  };
  ctx.health = [this] { return diagnostics_->run("diag-web").toJson(); };
  ctx.network = [this] {
    // `mode` ("wifi"/"ethernet") matters to the setup page: a speaker with an Ethernet uplink
    // reports connected=true even when a Wi-Fi join just failed, so "connected" alone would light
    // the success indicator on a Wi-Fi attempt that never worked. The UI needs to know WHICH link
    // came up before it can honestly say the Wi-Fi is connected.
    nlohmann::json j{{"connected", network_->isConnected()},
                     {"ip", network_->ipAddress()},
                     {"mode", network_->mode()}};
    // Streamer link telemetry: the paired streamer pushes its own Wi-Fi RSSI via REPORT_LINK. Expose
    // it (with a freshness flag) so the kiosk shows a per-streamer signal meter. `fresh` is false
    // once reports stop (streamer offline / no longer streaming), letting the kiosk fall back to its
    // own RTT-based link meter. Window: 10 s (streamer reports every ~2 s).
    const auto link = control_->linkState()->get();
    if (link.valid) {
      const std::int64_t now = static_cast<std::int64_t>(::time(nullptr));
      const bool fresh = (now - link.reported_at) <= 10;
      j["streamer_link"] = {{"wifi_signal_dbm", link.wifi_signal_dbm},
                            {"streamer_id", link.streamer_id},
                            {"reported_at", link.reported_at},
                            {"fresh", fresh}};
    }
    return j;
  };
  ctx.audio = [this] {
    const auto& c = config_->get();
    return nlohmann::json{{"volume", c.audio.volume},
                          {"muted", c.audio.muted},
                          {"delay_ms", c.audio.delay_ms},
                          {"eq_profile", c.audio.eq_profile}};
  };
  ctx.hardware = [this] {
    return nlohmann::json{{"amplifier", amplifier::toString(amplifier_->ampState())},
                          {"calibration", calibration::toString(calibration_->calState())},
                          {"audio_streaming", audio_->streaming()}};
  };
  ctx.calibrationStatus = [this] {
    return nlohmann::json{{"state", calibration::toString(calibration_->calState())}};
  };
  ctx.calibrationResult = [this] {
    return nlohmann::json{{"eq_profile", config_->get().audio.eq_profile}};
  };

  // Actions reuse the config/module logic; each returns (ok, message).
  ctx.setVolume = [this](const nlohmann::json& b) -> std::pair<bool, std::string> {
    if (!b.contains("volume") || !b["volume"].is_number_integer()) return {false, "need volume"};
    auto s = config_->update([&](config::SpeakerConfig& c) { c.audio.volume = b["volume"].get<int>(); });
    return {s.ok(), s.ok() ? "ok" : s.message()};
  };
  ctx.setMute = [this](const nlohmann::json& b) -> std::pair<bool, std::string> {
    if (!b.contains("muted") || !b["muted"].is_boolean()) return {false, "need muted"};
    auto s = config_->update([&](config::SpeakerConfig& c) { c.audio.muted = b["muted"].get<bool>(); });
    return {s.ok(), s.ok() ? "ok" : s.message()};
  };
  ctx.setDelay = [this](const nlohmann::json& b) -> std::pair<bool, std::string> {
    if (!b.contains("delay_ms") || !b["delay_ms"].is_number_integer()) return {false, "need delay_ms"};
    auto s = config_->update([&](config::SpeakerConfig& c) { c.audio.delay_ms = b["delay_ms"].get<int>(); });
    if (s.ok()) dsp_->setDelayMs(b["delay_ms"].get<int>());
    return {s.ok(), s.ok() ? "ok" : s.message()};
  };
  ctx.setEq = [this](const nlohmann::json& b) -> std::pair<bool, std::string> {
    std::string profile = b.value("eq_profile", std::string("default"));
    auto s = config_->update([&](config::SpeakerConfig& c) { c.audio.eq_profile = profile; });
    return {s.ok(), s.ok() ? "ok" : s.message()};
  };
  // Identify this speaker by making it — and only it — emit a sound.
  //
  // This was a stub that returned "audio test queued" and played nothing, so the UI's audio test
  // silently did nothing. During installation it is the ONLY way to tell which physical unit in the
  // room a row in the list refers to: the speakers have no addressable indicator LED (the GPIO map
  // is amp enable/mute plus fault/protect inputs — see GpioAmplifierHal::Pins), so sound is the
  // identification channel.
  //
  // The tone is pushed through the normal jitter buffer rather than written to the output HAL
  // directly: the playback thread owns that device, and a second writer would race it. Packets are
  // stamped with timestamp 0, which PlaybackManager treats as "not stamped by a sender — play
  // immediately" instead of holding them for a scheduled time that never arrives.
  //
  // AudioStarted/AudioStopped are published around it so the existing amp gating unmutes the
  // amplifier for the tone and mutes it again afterwards — without them a speaker that is idle
  // (amp muted, the normal resting state) would run the tone into a silent output.
  ctx.audioTest = [this]() -> std::pair<bool, std::string> { return playIdentifyTone(); };
  ctx.startCalibration = [this]() -> std::pair<bool, std::string> {
    auto r = calibration_->runCalibration("room");
    return {r.ok(), r.ok() ? "calibration complete" : r.status().message()};
  };
  ctx.reboot = [this]() -> std::pair<bool, std::string> {
    bus_.publish(core::Event{core::EventType::ShutdownRequested, "web"});
    return {true, "rebooting"};
  };
  ctx.update = []() -> std::pair<bool, std::string> { return {true, "update queued"}; };
  ctx.resetNetwork = [this]() -> std::pair<bool, std::string> {
    auto s = config_->update([](config::SpeakerConfig& c) {
      c.network.wifi_configured = false;
    });
    return {s.ok(), s.ok() ? "network reset" : s.message()};
  };
  ctx.factoryReset = [this]() -> std::pair<bool, std::string> {
    auto s = config_->resetToDefaults();  // preserves identity/keys
    return {s.ok(), s.ok() ? "factory reset (identity preserved)" : s.message()};
  };
  // Setup UI: list nearby Wi-Fi networks so the user can pick one.
  ctx.scanWifi = [this] {
    nlohmann::json list = nlohmann::json::array();
    auto res = network_->scan();
    if (res.ok()) {
      for (const auto& n : res.value()) {
        list.push_back({{"ssid", n.ssid}, {"signal_dbm", n.signal_dbm}});
      }
    }
    return nlohmann::json{{"networks", list}};
  };
  // Setup UI: join the chosen Wi-Fi. Body: {"ssid","psk"} plus optional advanced knobs:
  //   "hidden": bool, "band": "bg"|"a", "static_ip": "A.B.C.D/prefix", "gateway", "dns".
  ctx.connectWifi = [this](const nlohmann::json& body) -> std::pair<bool, std::string> {
    network::WifiConnectParams p;
    p.ssid = body.value("ssid", "");
    p.psk = body.value("psk", "");
    p.hidden = body.value("hidden", false);
    p.band = body.value("band", "");
    p.static_ip = body.value("static_ip", "");
    p.gateway = body.value("gateway", "");
    p.dns = body.value("dns", "");
    if (p.ssid.empty()) return {false, "ssid required"};
    // Guard the band value so only nmcli's accepted tokens ever reach the shell.
    if (!p.band.empty() && p.band != "bg" && p.band != "a") return {false, "band must be bg or a"};
    auto s = network_->connectWifi(p);
    // Record that Wi-Fi was configured (the flag resetNetwork clears); the credentials themselves
    // live in NetworkManager. Only on success — a failed join leaves the device unconfigured.
    // NOTE: the boot gate in startup() keys off pairing.paired, not this flag, so this alone does
    // not stop a reboot from re-entering SETUP_MODE — pairing is what completes onboarding.
    if (s.ok()) {
      config_->update([](config::SpeakerConfig& c) { c.network.wifi_configured = true; });
    }
    return {s.ok(), s.ok() ? "connecting to " + p.ssid : s.message()};
  };
  // Setup UI: set the speaker's display name (how it shows up to the streamer). Body: {"name": "..."}.
  ctx.setSpeakerName = [this](const nlohmann::json& body) -> std::pair<bool, std::string> {
    const std::string name = body.value("name", "");
    if (name.empty()) return {false, "name required"};
    auto s = config_->update([&](config::SpeakerConfig& c) { c.device.name = name; });
    return {s.ok(), s.ok() ? "name set" : s.message()};
  };
  return ctx;
}

Status Application::startup() {
  // Logger first, so every subsequent line is captured.
  logging::LogConfig lc;
  lc.file_path = opts_.log_path;
  logging::Logger::instance().init(lc);
  NX_LOG_INFO("main", "nexus-speaker starting");

  buildServices();

  Status s = startInOrder();
  if (!s.ok()) return s;

  // The maintenance token depends on the device id, which is only known after identity loads —
  // set it now (construction happened before identity was provisioned).
  web_->setAuthToken("nexus-" + identity_->deviceId());

  // Apply persisted DSP settings from config (delay; a protective limiter is on by default).
  const auto& cfg = config_->get();
  dsp_->setDelayMs(cfg.audio.delay_ms);
  dsp_->setLimiter(true, -1.0);
  // Per-speaker trim and polarity are installer settings: once a room is level-matched and phase
  // corrected, a reboot must not undo it.
  dsp_->setOutputGainDb(cfg.audio.gain_db);
  dsp_->setPhaseInvert(cfg.audio.phase_invert);

  // Re-apply a saved calibration profile so the room correction survives reboot.
  const std::string profile = cfg.audio.eq_profile;
  if (profile != "default" && cal_profiles_ && cal_profiles_->has(profile)) {
    Status cs = calibration_->applySavedProfile(profile);
    if (!cs.ok()) NX_LOG_WARN("main", "could not apply saved calibration: " + cs.message());
  }

  // Now that config + identity are up, move out of Booting.
  const bool configured = config_->get().pairing.paired;
  system_->enterInitialState(configured);

  if (!configured) {
    // Unpaired: enter setup mode, publish the mDNS beacon so a Streamer can find us, and open a
    // pairing window with a setup code. The code would normally be shown on a display / QR; here
    // it is derived deterministically from the device id so a technician can read it from the logs
    // or the web UI. (A production build shows it on-device and rotates it.)
    system_->states().transitionTo(system::SystemState::SetupMode, "unconfigured at boot");
    const std::string setup_code = "SETUP-" + identity_->deviceId().substr(4);
    auto now = static_cast<std::int64_t>(::time(nullptr));
    pairing_->beginSetupMode(setup_code, now, /*ttl_seconds=*/900);

    discovery::SpeakerBeacon beacon;
    beacon.device_id = identity_->deviceId();
    beacon.serial_number = identity_->serialNumber();
    beacon.model = config_->get().device.model;
    beacon.software_version = config_->get().device.software_version;
    beacon.box_public_key = keys_->boxPublicKeyBase64().value_or("");
    beacon.control_port = control::CommandServer::kDefaultPort;
    beacon.setup_mode = true;
    discovery_->advertise(beacon);
    NX_LOG_INFO("main", "SETUP MODE — setup code: " + setup_code + " (mDNS beacon advertised)");
  }

  NX_LOG_INFO("main", std::string("boot complete; state=") +
                          system::toString(system_->states().current()));
  return Status::success();
}

// Emit a short tone so an installer can tell which physical unit a row in the controller's list
// refers to. See the declaration in Application.h for why sound rather than an indicator LED.
//
// The tone goes through the normal jitter buffer instead of being written to the output device
// directly: the playback thread owns that device and a second writer would race it. Packets carry
// timestamp 0, which PlaybackManager treats as "not stamped by a sender — play immediately" rather
// than holding them for a scheduled time that never arrives.
//
// AudioStarted is published first so the existing amp gating unmutes the amplifier; an idle speaker
// rests with the amp muted, and without this the tone would play into a silent output. The amp is
// left unmuted afterwards exactly as it is after any stream — AudioStopped is what mutes it, and
// that fires from the normal playback path.
std::pair<bool, std::string> Application::playIdentifyTone() {
  if (config_->get().audio.muted) return {false, "speaker is muted"};

  constexpr int kSampleRate = 48000;
  constexpr int kChannels = 2;
  constexpr double kFreqHz = 880.0;      // clear, well inside every driver's range
  constexpr double kSeconds = 1.0;
  constexpr double kAmplitude = 0.25;    // audible without being startling at high volume
  constexpr int kFramesPerPacket = 480;  // 10 ms — the same packet size the network path uses
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kFadeSeconds = 0.005;  // avoids a click at the start and end

  bus_.publish(core::Event{core::EventType::AudioStarted, "identify"});

  const int packets = static_cast<int>(kSeconds * kSampleRate / kFramesPerPacket);
  for (int p = 0; p < packets; ++p) {
    audio::AudioPacket pkt;
    pkt.header.timestamp = 0.0;
    pkt.header.sequence = static_cast<std::uint32_t>(p);
    pkt.header.frame_count = static_cast<std::uint16_t>(kFramesPerPacket);
    pkt.header.flags = 0;
    pkt.samples.resize(static_cast<std::size_t>(kFramesPerPacket) * kChannels);
    for (int f = 0; f < kFramesPerPacket; ++f) {
      const double t = static_cast<double>(p * kFramesPerPacket + f) / kSampleRate;
      double env = 1.0;
      if (t < kFadeSeconds) env = t / kFadeSeconds;
      const double remaining = kSeconds - t;
      if (remaining < kFadeSeconds) env = std::max(0.0, remaining / kFadeSeconds);
      const auto s = static_cast<std::int16_t>(kAmplitude * env * 32767.0 *
                                               std::sin(2.0 * kPi * kFreqHz * t));
      for (int c = 0; c < kChannels; ++c) {
        pkt.samples[static_cast<std::size_t>(f) * kChannels + c] = s;
      }
    }
    audio_buffer_.push(std::move(pkt));
  }
  NX_LOG_INFO("identify", "playing identify tone");
  return {true, "playing identify tone"};
}

Status Application::startInOrder() {
  for (auto* svc : ordered_) {
    Status s = svc->start();
    if (s.ok()) {
      started_.push_back(svc);
      continue;
    }
    // Critical services abort the boot; everything else degrades and we continue.
    const bool critical = (svc == config_.get()) || (svc == identity_.get());
    if (critical) {
      NX_LOG_CRIT("main", ErrorCode::ServiceStartFailed,
                  std::string("critical service '") + svc->name() + "' failed: " + s.message());
      shutdown();
      return Status::error(ErrorCode::ServiceStartFailed, svc->name());
    }
    NX_LOG_ERROR("main", ErrorCode::ServiceStartFailed,
                 std::string("service '") + svc->name() + "' degraded: " + s.message());
    started_.push_back(svc);  // still track so stop() runs
  }
  return Status::success();
}

void Application::requestShutdown() {
  shutdown_requested_ = true;
  bus_.publish(core::Event{core::EventType::ShutdownRequested, "main"});
  g_wait_cv.notify_all();
}

int Application::run() {
  {
    std::unique_lock<std::mutex> lock(g_wait_mutex);
    g_wait_cv.wait(lock, [this] { return shutdown_requested_.load(); });
  }
  NX_LOG_INFO("main", "shutdown requested; stopping services");
  shutdown();
  return 0;
}

void Application::shutdown() {
  if (stopped_) return;
  stopped_ = true;
  // Reverse startup order.
  for (auto it = started_.rbegin(); it != started_.rend(); ++it) {
    Status s = (*it)->stop();
    if (!s.ok()) {
      NX_LOG_WARN("main", std::string("service '") + (*it)->name() + "' stop error: " +
                              s.message());
    }
  }
  started_.clear();
  bus_.stop();
  logging::Logger::instance().flush();
  logging::Logger::instance().shutdown();
}

}  // namespace nexus
