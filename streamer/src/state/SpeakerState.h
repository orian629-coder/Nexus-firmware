#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace nexus::streamer::state {

// What a speaker actually reported about itself.
//
// Deliberately a separate type from group::Speaker (identity + addressing), so "who this speaker is
// and how to reach it" can never be confused with "what it told us its settings are". The former is
// configuration the streamer owns; the latter is owned by the speaker and merely mirrored here.
//
// Every field is written ONLY from a speaker reply. There is no constructor or setter that takes a
// user-requested value, which is what makes the rule "the UI never assumes a parameter changed"
// enforceable rather than aspirational.
struct ConfirmedState {
  int volume = 0;
  bool muted = false;
  int delay_ms = 0;
  std::string eq_profile;
  double gain_db = 0.0;        // per-speaker trim, separate from `volume`
  bool phase_invert = false;   // polarity flip for a driver that cancels its neighbours
  bool paired = false;

  // Playback health as the speaker last reported it: buffer depth, packet loss, underflows,
  // latency. Held as opaque JSON so a speaker running newer firmware can report more fields without
  // a streamer change — the UI renders what it recognizes. Empty when the speaker reported none
  // (older firmware, or a build with no audio pipeline), which the UI must show as "no data" rather
  // than as a healthy stream of zeros.
  nlohmann::json telemetry = nlohmann::json::object();

  // The streamer's link strength as the speaker last saw it, with the age of that report. A stale
  // RSSI is worse than none: REPORT_LINK stops arriving when the link dies, so a frozen value would
  // keep showing a strong signal for a connection that no longer exists.
  bool has_link = false;
  int wifi_signal_dbm = 0;
  std::int64_t link_age_s = -1;  // -1 = unknown age

  // When the speaker last confirmed any of the above. Zero means "never heard from".
  std::chrono::steady_clock::time_point confirmed_at{};

  bool everConfirmed() const { return confirmed_at.time_since_epoch().count() != 0; }
};

// The streamer's view of one speaker: what it reported, plus reachability derived from polling.
struct SpeakerState {
  std::string device_id;
  ConfirmedState confirmed;

  // Reachability, derived from poll outcomes only — never from a user action. A speaker that dies
  // silently must go offline on its own, which the pre-Phase-2 code could not do because `online`
  // was only touched as a side effect of a UI-triggered command.
  bool online = false;
  int consecutive_failures = 0;
  std::string last_error;    // why the last exchange failed (transport error or speaker rejection)

  // Age of the newest confirmation. Used by the UI to show a value as stale rather than as truth.
  std::chrono::milliseconds age(std::chrono::steady_clock::time_point now) const {
    if (!confirmed.everConfirmed()) return std::chrono::milliseconds::max();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - confirmed.confirmed_at);
  }

  nlohmann::json toJson(std::chrono::steady_clock::time_point now) const {
    nlohmann::json j{
        {"device_id", device_id},
        {"online", online},
        {"confirmed", confirmed.everConfirmed()},
    };
    if (!last_error.empty()) j["last_error"] = last_error;
    if (confirmed.everConfirmed()) {
      j["volume"] = confirmed.volume;
      j["muted"] = confirmed.muted;
      j["delay_ms"] = confirmed.delay_ms;
      j["eq_profile"] = confirmed.eq_profile;
      j["gain_db"] = confirmed.gain_db;
      j["phase_invert"] = confirmed.phase_invert;
      j["paired"] = confirmed.paired;
      j["age_ms"] = static_cast<std::int64_t>(age(now).count());
      // Omitted entirely when the speaker reported nothing, so the UI can tell "no telemetry" from
      // "telemetry that happens to read zero".
      if (!confirmed.telemetry.empty()) j["telemetry"] = confirmed.telemetry;
      if (confirmed.has_link) {
        j["wifi_signal_dbm"] = confirmed.wifi_signal_dbm;
        if (confirmed.link_age_s >= 0) j["link_age_s"] = confirmed.link_age_s;
      }
    }
    return j;
  }
};

}  // namespace nexus::streamer::state
