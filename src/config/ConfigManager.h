#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "config/SpeakerConfig.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "core/Result.h"

namespace nexus::config {

// Loads, validates, and atomically persists the speaker configuration.
//
// Guarantees:
//  - Atomic writes: temp file -> fsync -> rename -> fsync(dir). A crash never leaves a partial
//    config.json.
//  - A one-deep backup (config.json.bak) is kept before each overwrite.
//  - Validation gate: every update()/save() validates BEFORE persisting; invalid input is
//    rejected and the on-disk file is left untouched.
//  - Corruption recovery on load: fall back to config.json.bak, else seed defaults, and emit a
//    ConfigInvalid event so SystemManager can react (Safe Mode in a later phase).
//  - No secrets: the config contains no passwords/keys.
class ConfigManager : public core::IService {
 public:
  explicit ConfigManager(std::string path = "/etc/nexus-speaker/config.json",
                         core::EventBus* bus = nullptr);

  std::string name() const override { return "config"; }
  core::Status start() override;  // load(); if missing, seed defaults + persist
  core::Status stop() override;
  core::ServiceState state() const override;

  // Thread-safe snapshot of the current config.
  SpeakerConfig get() const;

  core::Status load();
  core::Status reloadFromDisk();

  // Apply a mutation, validate the result, then atomically persist. On validation failure the
  // in-memory and on-disk config are both left unchanged.
  core::Status update(const std::function<void(SpeakerConfig&)>& mutator);

  // Restore defaults and persist (used by factory reset — never touches identity/secrets).
  core::Status resetToDefaults();

 private:
  core::Status persist(const SpeakerConfig& cfg);  // validated atomic write + backup

  std::string path_;
  core::EventBus* bus_;
  mutable std::mutex mutex_;
  SpeakerConfig config_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::config
