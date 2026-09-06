#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/EventBus.h"
#include "core/StreamerEvents.h"
#include "state/SpeakerState.h"

namespace nexus::streamer::state {

// Holds what every speaker has actually confirmed about itself.
//
// The API is the design: there is NO generic setter. The only mutators take either a speaker's
// reply or a failure, so no code path — not even a careless one — can write a value the user merely
// requested. That is what makes "the speaker is the source of truth" a property of the type rather
// than a rule people have to remember.
//
// Thread-safe: the monitor thread writes while the web thread reads.
class SpeakerStateStore {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  explicit SpeakerStateStore(core::EventBus* bus = nullptr, Clock clock = nullptr)
      : bus_(bus),
        clock_(clock ? std::move(clock)
                     : Clock([] { return std::chrono::steady_clock::now(); })) {}

  // Apply the `data` object from a successful speaker reply (GET_STATUS, or the echo that
  // SET_VOLUME/SET_MUTE/SET_DELAY/SET_EQ return). Only keys the speaker actually sent are copied,
  // so a partial reply updates only what it mentions.
  //
  // This is also why the speaker's nine "deferred" commands are safe: they reply
  // {"accepted":true,"deferred":true}, which contains no state keys, so nothing is marked confirmed
  // by a command that did nothing.
  void applyConfirmedStatus(const std::string& device_id, const nlohmann::json& data) {
    bool became_online = false;
    nlohmann::json changed = nlohmann::json::object();
    {
      std::lock_guard<std::mutex> lk(mutex_);
      auto& s = states_[device_id];
      s.device_id = device_id;

      bool touched = false;
      if (data.is_object()) {
        if (data.contains("volume") && data["volume"].is_number_integer()) {
          const int v = data["volume"].get<int>();
          if (!s.confirmed.everConfirmed() || s.confirmed.volume != v) changed["volume"] = v;
          s.confirmed.volume = v;
          touched = true;
        }
        if (data.contains("muted") && data["muted"].is_boolean()) {
          const bool m = data["muted"].get<bool>();
          if (!s.confirmed.everConfirmed() || s.confirmed.muted != m) changed["muted"] = m;
          s.confirmed.muted = m;
          touched = true;
        }
        if (data.contains("delay_ms") && data["delay_ms"].is_number_integer()) {
          s.confirmed.delay_ms = data["delay_ms"].get<int>();
          touched = true;
        }
        if (data.contains("eq_profile") && data["eq_profile"].is_string()) {
          s.confirmed.eq_profile = data["eq_profile"].get<std::string>();
          touched = true;
        }
        if (data.contains("gain_db") && data["gain_db"].is_number()) {
          const double g = data["gain_db"].get<double>();
          if (!s.confirmed.everConfirmed() || s.confirmed.gain_db != g) changed["gain_db"] = g;
          s.confirmed.gain_db = g;
          touched = true;
        }
        if (data.contains("phase_invert") && data["phase_invert"].is_boolean()) {
          const bool inv = data["phase_invert"].get<bool>();
          if (!s.confirmed.everConfirmed() || s.confirmed.phase_invert != inv) {
            changed["phase_invert"] = inv;
          }
          s.confirmed.phase_invert = inv;
          touched = true;
        }
        if (data.contains("paired") && data["paired"].is_boolean()) {
          s.confirmed.paired = data["paired"].get<bool>();
          touched = true;
        }
        // Stored opaquely: whatever health fields this speaker's firmware reports pass through, so
        // a newer speaker can add metrics without a streamer change.
        if (data.contains("telemetry") && data["telemetry"].is_object()) {
          s.confirmed.telemetry = data["telemetry"];
          touched = true;
        }
        if (data.contains("wifi_signal_dbm") && data["wifi_signal_dbm"].is_number_integer()) {
          s.confirmed.has_link = true;
          s.confirmed.wifi_signal_dbm = data["wifi_signal_dbm"].get<int>();
          s.confirmed.link_age_s =
              (data.contains("link_age_s") && data["link_age_s"].is_number_integer())
                  ? data["link_age_s"].get<std::int64_t>()
                  : -1;
          touched = true;
        }
      }
      if (touched) s.confirmed.confirmed_at = clock_();

      // Any successful exchange proves reachability, even one carrying no state keys.
      became_online = !s.online;
      s.online = true;
      s.consecutive_failures = 0;
      s.last_error.clear();
    }
    if (became_online && bus_) {
      bus_->publish(events::make(events::kSpeakerOnline, {{"device_id", device_id}}));
    }
    if (!changed.empty() && bus_) {
      changed["device_id"] = device_id;
      bus_->publish(events::make(events::kSpeakerState, changed));
    }
  }

  // Record a failed exchange: a transport error, or a speaker that replied ok:false. Confirmed
  // values are left untouched — a failure tells us nothing new about the speaker's settings, only
  // about our ability to reach it. After `offline_threshold` consecutive failures the speaker is
  // marked offline.
  void applyPollFailure(const std::string& device_id, const std::string& why,
                        int offline_threshold = 3) {
    bool became_offline = false;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      auto& s = states_[device_id];
      s.device_id = device_id;
      s.last_error = why;
      ++s.consecutive_failures;
      if (s.online && s.consecutive_failures >= offline_threshold) {
        s.online = false;
        became_offline = true;
      }
    }
    if (became_offline && bus_) {
      bus_->publish(events::make(events::kSpeakerOffline, {{"device_id", device_id}, {"why", why}}));
    }
  }

  // A speaker that rejected a command (replied ok:false) is reachable but refused. Distinct from a
  // transport failure: it must NOT count toward going offline, and must not update confirmed state.
  void noteRejected(const std::string& device_id, const std::string& message) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto& s = states_[device_id];
    s.device_id = device_id;
    s.last_error = message;
    s.online = true;  // it answered, so it is alive
    s.consecutive_failures = 0;
  }

  void forget(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    states_.erase(device_id);
  }

  std::optional<SpeakerState> get(const std::string& device_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = states_.find(device_id);
    if (it == states_.end()) return std::nullopt;
    return it->second;
  }

  std::vector<SpeakerState> list() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<SpeakerState> out;
    out.reserve(states_.size());
    for (const auto& [_, s] : states_) out.push_back(s);
    return out;
  }

  std::chrono::steady_clock::time_point now() const { return clock_(); }

 private:
  core::EventBus* bus_;
  Clock clock_;
  mutable std::mutex mutex_;
  std::map<std::string, SpeakerState> states_;
};

}  // namespace nexus::streamer::state
