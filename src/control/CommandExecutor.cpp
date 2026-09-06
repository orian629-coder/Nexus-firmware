#include "control/CommandExecutor.h"

#include "logging/Logger.h"

namespace nexus::control {

using core::ErrorCode;
using core::Event;
using core::EventType;

CommandExecutor::CommandExecutor(core::EventBus* bus, config::ConfigManager* config,
                                 LinkState* link_state, std::function<std::int64_t()> now)
    : bus_(bus), config_(config), link_state_(link_state), now_(std::move(now)) {}

CommandResult CommandExecutor::ok(const Command& c, nlohmann::json data) {
  CommandResult r;
  r.command_id = c.command_id;
  r.ok = true;
  r.message = "ok";
  r.data = std::move(data);
  return r;
}

CommandResult CommandExecutor::fail(const Command& c, ErrorCode code, const std::string& msg) {
  CommandResult r;
  r.command_id = c.command_id;
  r.ok = false;
  r.message = msg;
  r.error_code = core::toInt(code);
  return r;
}

CommandResult CommandExecutor::execute(const Command& c) {
  NX_LOG_INFO("control", "execute " + c.command + " [cmd=" + c.command_id + "]");

  // Announce that a command is being handled (StatusService/telemetry may observe this).
  if (bus_) {
    bus_->publish(Event{EventType::CommandReceived, "control", {{"command", c.command}}});
  }

  const std::string& cmd = c.command;

  auto emit_executed = [&](nlohmann::json extra = nlohmann::json::object()) {
    if (bus_) {
      Event e{EventType::CommandExecuted, "control", {{"command", cmd}}};
      e.command_id = c.command_id;
      for (auto& [k, v] : extra.items()) e.data[k] = v;
      bus_->publish(std::move(e));
    }
  };

  if (cmd == cmd::kGetStatus) {
    const auto& cfg = config_->get();
    nlohmann::json status{{"volume", cfg.audio.volume},
                          {"muted", cfg.audio.muted},
                          {"delay_ms", cfg.audio.delay_ms},
                          {"eq_profile", cfg.audio.eq_profile},
                          {"gain_db", cfg.audio.gain_db},
                          {"phase_invert", cfg.audio.phase_invert},
                          {"paired", cfg.pairing.paired}};
    // Playback health, when this build has an audio pipeline. Absent rather than zeroed when there
    // is none: all-zero counters are indistinguishable from a flawless stream, and the streamer
    // would draw a healthy meter for a speaker that is not playing at all.
    if (telemetry_) {
      auto t = telemetry_();
      if (t.is_object() && !t.empty()) status["telemetry"] = std::move(t);
    }
    // The streamer's own link report, echoed back so one poll answers "can I reach it" and "how
    // good is its link" together. Reported with its age rather than as a bare number: REPORT_LINK
    // stops arriving the moment the streamer goes away, and a frozen RSSI would keep showing a
    // strong signal for a link that no longer exists.
    if (link_state_) {
      const auto link = link_state_->get();
      if (link.valid) {
        status["wifi_signal_dbm"] = link.wifi_signal_dbm;
        const std::int64_t now = now_ ? now_() : 0;
        if (now > 0 && link.reported_at > 0) {
          status["link_age_s"] = now - link.reported_at;
        }
      }
    }
    return ok(c, std::move(status));
  }

  if (cmd == cmd::kSetVolume) {
    if (!c.payload.contains("volume") || !c.payload["volume"].is_number_integer()) {
      return fail(c, ErrorCode::InvalidArg, "SET_VOLUME requires integer 'volume'");
    }
    int vol = c.payload["volume"].get<int>();
    auto s = config_->update([&](config::SpeakerConfig& cfg) { cfg.audio.volume = vol; });
    if (!s.ok()) return fail(c, s.code(), s.message());
    emit_executed({{"volume", vol}});
    return ok(c, {{"volume", vol}});
  }

  if (cmd == cmd::kSetMute) {
    if (!c.payload.contains("muted") || !c.payload["muted"].is_boolean()) {
      return fail(c, ErrorCode::InvalidArg, "SET_MUTE requires boolean 'muted'");
    }
    bool muted = c.payload["muted"].get<bool>();
    auto s = config_->update([&](config::SpeakerConfig& cfg) { cfg.audio.muted = muted; });
    if (!s.ok()) return fail(c, s.code(), s.message());
    emit_executed({{"muted", muted}});
    return ok(c, {{"muted", muted}});
  }

  if (cmd == cmd::kSetDelay) {
    if (!c.payload.contains("delay_ms") || !c.payload["delay_ms"].is_number_integer()) {
      return fail(c, ErrorCode::InvalidArg, "SET_DELAY requires integer 'delay_ms'");
    }
    int delay = c.payload["delay_ms"].get<int>();
    auto s = config_->update([&](config::SpeakerConfig& cfg) { cfg.audio.delay_ms = delay; });
    if (!s.ok()) return fail(c, s.code(), s.message());
    emit_executed({{"delay_ms", delay}});
    return ok(c, {{"delay_ms", delay}});
  }

  if (cmd == cmd::kSetEq) {
    // Persist the requested profile name; the DSP module (Phase 5) applies the bands.
    std::string profile = c.payload.value("eq_profile", std::string("default"));
    auto s = config_->update([&](config::SpeakerConfig& cfg) { cfg.audio.eq_profile = profile; });
    if (!s.ok()) return fail(c, s.code(), s.message());
    emit_executed({{"eq_profile", profile}});
    return ok(c, {{"eq_profile", profile}});
  }

  // Acoustic measurement: emit a chirp from this speaker and report what its microphones heard.
  //
  // Refused outright when no runner is wired, rather than acknowledged as "accepted". A measurement
  // that silently returns nothing would be indistinguishable from a room where the mics cannot hear
  // the speaker, and the installer would go looking for the fault in the wrong place.
  if (cmd == cmd::kRunMeasurement) {
    if (!measurement_) {
      return fail(c, ErrorCode::NotImplemented,
                  "this speaker has no measurement capability (no microphone configured)");
    }
    nlohmann::json result = measurement_(c.payload);
    emit_executed();
    // The runner reports its own success: a measurement can legitimately fail (silent room, mic
    // unplugged) without the command itself being invalid, and those are different things.
    return ok(c, std::move(result));
  }

  // Per-speaker trim, applied in the DSP output stage. Distinct from SET_VOLUME (the listener's
  // 0..100 control): this level-matches one speaker against the others once, and stays applied at
  // every volume setting.
  if (cmd == cmd::kSetGain) {
    if (!c.payload.contains("gain_db") || !c.payload["gain_db"].is_number()) {
      return fail(c, ErrorCode::InvalidArg, "SET_GAIN requires numeric 'gain_db'");
    }
    double gain = c.payload["gain_db"].get<double>();
    // Clamped rather than rejected: an out-of-range request is a slider overshoot, not a protocol
    // error, and the reply reports what was actually applied so the UI can snap back to it.
    if (gain > 20.0) gain = 20.0;
    if (gain < -20.0) gain = -20.0;
    auto s = config_->update([&](config::SpeakerConfig& cfg) { cfg.audio.gain_db = gain; });
    if (!s.ok()) return fail(c, s.code(), s.message());
    emit_executed({{"gain_db", gain}});
    return ok(c, {{"gain_db", gain}});
  }

  if (cmd == cmd::kSetPhaseInvert) {
    if (!c.payload.contains("phase_invert") || !c.payload["phase_invert"].is_boolean()) {
      return fail(c, ErrorCode::InvalidArg, "SET_PHASE_INVERT requires boolean 'phase_invert'");
    }
    const bool inv = c.payload["phase_invert"].get<bool>();
    auto s = config_->update([&](config::SpeakerConfig& cfg) { cfg.audio.phase_invert = inv; });
    if (!s.ok()) return fail(c, s.code(), s.message());
    emit_executed({{"phase_invert", inv}});
    return ok(c, {{"phase_invert", inv}});
  }

  // Commands whose executing modules arrive in later phases: acknowledge and emit the intent so
  // those modules act when they come online. This keeps the control contract stable now.
  if (cmd == cmd::kStartAudio || cmd == cmd::kStopAudio ||
      cmd == cmd::kPauseAudio || cmd == cmd::kResumeAudio || cmd == cmd::kRunCalibration ||
      cmd == cmd::kRunSelfTest || cmd == cmd::kRunAudioTest || cmd == cmd::kUpdateSoftware) {
    emit_executed();
    return ok(c, {{"accepted", true}, {"deferred", true}});
  }

  if (cmd == cmd::kReboot || cmd == cmd::kResetNetwork || cmd == cmd::kFactoryReset) {
    // These are potentially destructive lifecycle actions. Emit an event; the responsible modules
    // (system/network) carry them out. For REBOOT we also signal shutdown intent.
    emit_executed();
    if (cmd == cmd::kReboot && bus_) {
      bus_->publish(Event{EventType::ShutdownRequested, "control"});
    }
    return ok(c, {{"accepted", true}});
  }

  if (cmd == cmd::kReportLink) {
    // Streamer link telemetry: record the reported RSSI (with a timestamp for freshness) so the
    // kiosk's per-streamer signal meter can show it. Ephemeral — never persisted. A missing/invalid
    // signal is a soft error (the streamer may be wired); we still ack so it isn't retried.
    if (c.payload.contains("wifi_signal_dbm") && c.payload["wifi_signal_dbm"].is_number_integer()) {
      const int dbm = c.payload["wifi_signal_dbm"].get<int>();
      const std::string sid = c.payload.value("streamer_id", std::string());
      if (link_state_) link_state_->report(dbm, sid, now_ ? now_() : 0);
      return ok(c, {{"recorded", true}, {"wifi_signal_dbm", dbm}});
    }
    return ok(c, {{"recorded", false}});
  }

  return fail(c, ErrorCode::NotFound, "unknown command: " + cmd);
}

}  // namespace nexus::control
