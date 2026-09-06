#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nexus::streamer::group {

// One speaker the streamer controls: its identity, where to reach its control channel, and a UI
// label. The audio fan-out target (ip:50005) is derived from `host`. This is the in-memory model
// behind the web UI's speaker list and the eventual multi-room GroupManager — the same set of
// speakers that receive commands is the set that receives the synchronized audio stream.
//
// This struct is ADDRESSING ONLY: who the speaker is and how to reach it, which the streamer owns.
// Anything the speaker reports about itself — volume, mute, EQ, and whether it is even reachable —
// lives in state::SpeakerStateStore and is written only from the speaker's own replies. The `online`
// flag used to live here and was updated as a side effect of user-initiated commands, which meant a
// speaker that died silently stayed green until someone clicked something. Keeping the two apart
// makes that class of bug unrepresentable rather than merely fixed.
struct Speaker {
  std::string device_id;   // command target_id (SPK-XXXX)
  std::string name;        // human label ("Living Room")
  std::string host;        // resolved IPv4 / hostname for control + audio
  int control_port = 45455;
};

// Thread-safe collection of speakers the streamer manages. Registration order is preserved so the
// UI list is stable. All mutations and reads are guarded so the web thread and a future
// status-poll thread can share it.
class SpeakerRegistry {
 public:
  // Add or update a speaker (keyed by device_id). Returns true if newly added.
  bool upsert(const Speaker& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : speakers_) {
      if (e.device_id == s.device_id) {
        e = s;
        return false;
      }
    }
    speakers_.push_back(s);
    return true;
  }

  bool remove(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = speakers_.begin(); it != speakers_.end(); ++it) {
      if (it->device_id == device_id) {
        speakers_.erase(it);
        return true;
      }
    }
    return false;
  }

  std::optional<Speaker> get(const std::string& device_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& e : speakers_) {
      if (e.device_id == device_id) return e;
    }
    return std::nullopt;
  }

  std::vector<Speaker> list() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return speakers_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return speakers_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Speaker> speakers_;
};

}  // namespace nexus::streamer::group
