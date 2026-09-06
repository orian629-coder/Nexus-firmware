#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

#include "audio/AudioPacket.h"
#include "core/Result.h"
#include "group/SpeakerRegistry.h"
#include "group/Zone.h"
#include "state/SpeakerStateStore.h"

namespace nexus::streamer::group {

// Where one audio datagram is sent. Mirrors audio::Endpoint but declared here so the group module
// does not depend on the audio module (the dependency runs the other way: the app resolves a zone
// into endpoints and hands them to the engine).
struct ZoneEndpoint {
  std::string host;
  int port = nexus::audio::wire::kDefaultPort;
};

// Thread-safe CRUD over zones, plus the two resolutions that make a zone useful.
//
// Enforces one invariant: a speaker belongs to at most one zone. Two zones containing the same
// speaker could each try to stream to it, and the speaker has no way to arbitrate — it would just
// play whichever packets arrived last, producing audible garbage. Rejecting it here gives the user a
// clear error instead.
class ZoneManager {
 public:
  // `id_source` generates zone ids; injected so tests are deterministic (no clock/random in logic).
  using IdSource = std::function<std::string()>;

  ZoneManager(SpeakerRegistry& registry, const state::SpeakerStateStore* store = nullptr,
              IdSource id_source = nullptr)
      : registry_(registry), store_(store), id_source_(std::move(id_source)) {}

  core::Result<Zone> create(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    Zone z;
    // Skip ids already taken. `next_` counts from 1 in memory, but zones are restored from disk on
    // boot, so without this the first zone created after a restart reuses "zone-1" — and since
    // remove()/members()/play() all match by id, the user's action would silently hit the OTHER
    // zone. Also covers an injected id_source that repeats.
    for (int guard = 0; guard < 10000; ++guard) {
      std::string candidate = id_source_ ? id_source_() : ("zone-" + std::to_string(next_++));
      const bool taken = std::any_of(zones_.begin(), zones_.end(), [&](const Zone& existing) {
        return existing.zone_id == candidate;
      });
      if (!taken) {
        z.zone_id = std::move(candidate);
        break;
      }
      // A deterministic id_source that keeps returning the same value would loop forever; give up
      // rather than hang, and report it instead of creating a duplicate.
      if (id_source_ && guard > 0) {
        return core::Status::error(core::ErrorCode::AlreadyExists,
                                   "zone id already exists: " + candidate);
      }
    }
    if (z.zone_id.empty()) {
      return core::Status::error(core::ErrorCode::AlreadyExists, "could not allocate a zone id");
    }
    z.name = name.empty() ? z.zone_id : name;
    zones_.push_back(z);
    return z;
  }

  bool remove(const std::string& zone_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    const auto before = zones_.size();
    zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                                [&](const Zone& z) { return z.zone_id == zone_id; }),
                 zones_.end());
    return zones_.size() != before;
  }

  core::Status rename(const std::string& zone_id, const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& z : zones_) {
      if (z.zone_id == zone_id) {
        z.name = name;
        return core::Status::success();
      }
    }
    return core::Status::error(core::ErrorCode::NotFound, "unknown zone: " + zone_id);
  }

  // Add a speaker to a zone. Fails if it already belongs to a different one.
  core::Status addMember(const std::string& zone_id, const std::string& device_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!registry_.get(device_id)) {
      return core::Status::error(core::ErrorCode::NotFound, "unknown speaker: " + device_id);
    }
    for (const auto& z : zones_) {
      if (z.zone_id == zone_id) continue;
      if (std::find(z.members.begin(), z.members.end(), device_id) != z.members.end()) {
        return core::Status::error(core::ErrorCode::InvalidArg,
                                   device_id + " already belongs to zone " + z.name);
      }
    }
    for (auto& z : zones_) {
      if (z.zone_id != zone_id) continue;
      if (std::find(z.members.begin(), z.members.end(), device_id) == z.members.end()) {
        z.members.push_back(device_id);
      }
      return core::Status::success();
    }
    return core::Status::error(core::ErrorCode::NotFound, "unknown zone: " + zone_id);
  }

  core::Status removeMember(const std::string& zone_id, const std::string& device_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& z : zones_) {
      if (z.zone_id != zone_id) continue;
      z.members.erase(std::remove(z.members.begin(), z.members.end(), device_id), z.members.end());
      return core::Status::success();
    }
    return core::Status::error(core::ErrorCode::NotFound, "unknown zone: " + zone_id);
  }

  std::optional<Zone> get(const std::string& zone_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& z : zones_) {
      if (z.zone_id == zone_id) return z;
    }
    return std::nullopt;
  }

  std::vector<Zone> list() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return zones_;
  }

  // Replace the whole set (used when hydrating from persisted config at boot).
  // Restores zones loaded from disk. Also advances the id counter past anything already used, so
  // ids stay monotonic across a restart instead of restarting at 1 and colliding with the zones
  // just loaded. create() re-checks for collisions regardless; this keeps the common case from
  // having to iterate at all.
  void replaceAll(std::vector<Zone> zones) {
    std::lock_guard<std::mutex> lk(mutex_);
    zones_ = std::move(zones);
    for (const auto& z : zones_) {
      // Parse the "zone-N" ids this class generates; ids from an injected source are left alone.
      if (z.zone_id.rfind("zone-", 0) != 0) continue;
      const std::string digits = z.zone_id.substr(5);
      if (digits.empty() ||
          !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
        continue;
      }
      try {
        next_ = std::max(next_, std::stoi(digits) + 1);
      } catch (const std::exception&) {
        // Out of int range: keep the current counter and let create()'s collision check handle it.
      }
    }
  }

  // AUDIO fan-out: skips members that are known offline — streaming to a host that is not answering
  // wastes bandwidth on every 10 ms block and can never produce sound.
  core::Result<std::vector<ZoneEndpoint>> resolveTargets(const std::string& zone_id) const {
    auto zone = get(zone_id);
    if (!zone) return core::Status::error(core::ErrorCode::NotFound, "unknown zone: " + zone_id);

    std::vector<ZoneEndpoint> out;
    for (const auto& device_id : zone->members) {
      auto sp = registry_.get(device_id);
      if (!sp) continue;
      if (store_) {
        auto st = store_->get(device_id);
        if (st && !st->online) continue;  // known offline → not an audio target
      }
      out.push_back({sp->host, nexus::audio::wire::kDefaultPort});
    }
    return out;
  }

  // COMMAND fan-out: includes offline members on purpose, so the caller gets a real per-speaker
  // failure to show the user rather than the command silently applying to fewer rooms than asked.
  std::vector<Speaker> members(const std::string& zone_id) const {
    std::vector<Speaker> out;
    auto zone = get(zone_id);
    if (!zone) return out;
    for (const auto& device_id : zone->members) {
      if (auto sp = registry_.get(device_id)) out.push_back(*sp);
    }
    return out;
  }

  // Which zone a speaker belongs to, if any. Used by the UI to show a badge on each speaker row.
  std::optional<std::string> zoneOf(const std::string& device_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& z : zones_) {
      if (std::find(z.members.begin(), z.members.end(), device_id) != z.members.end()) {
        return z.zone_id;
      }
    }
    return std::nullopt;
  }

 private:
  SpeakerRegistry& registry_;
  const state::SpeakerStateStore* store_;
  IdSource id_source_;
  mutable std::mutex mutex_;
  std::vector<Zone> zones_;
  int next_ = 1;
};

}  // namespace nexus::streamer::group
