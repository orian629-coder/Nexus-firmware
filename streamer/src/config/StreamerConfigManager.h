#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "config/StreamerConfig.h"
#include "core/IService.h"
#include "core/Result.h"

namespace nexus::streamer::config {

// Owns the streamer's on-disk config: load at boot, atomic save on every change, and recovery from
// a corrupt file via a `.bak` sibling.
//
// Before this existed the speaker list lived only in memory, so every restart forgot which speakers
// were paired and the user had to re-add them. Modeled on the speaker's config::ConfigManager and
// reusing its exact atomic-write primitive (core::atomicWriteFile), because a half-written config
// after a power cut is the difference between a streamer that boots and one that does not.
//
// What is persisted is deliberately narrow: identity, speakers (addressing), zones, and settings.
// Never anything a speaker reported about itself — that lives in state::SpeakerStateStore and must
// always come fresh from the device.
class StreamerConfigManager : public core::IService {
 public:
  explicit StreamerConfigManager(std::string path);

  std::string name() const override { return "streamer-config"; }
  core::Status start() override;  // loads from disk (or seeds defaults on first run)
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Snapshot of the current config. Cheap; returns a copy so callers never hold the lock.
  StreamerConfig get() const;

  // Mutate under lock, then persist atomically. The mutation is kept in memory even if the write
  // fails, so a read-only disk degrades to "settings don't survive restart" rather than
  // "settings don't apply".
  core::Status update(const std::function<void(StreamerConfig&)>& mutator);

  const std::string& path() const { return path_; }

 private:
  core::Status saveLocked() const;

  std::string path_;
  mutable std::mutex mutex_;
  StreamerConfig config_;
  std::atomic<core::ServiceState> state_{core::ServiceState::Stopped};
};

}  // namespace nexus::streamer::config
