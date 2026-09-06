#include "config/ConfigManager.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "config/ConfigSerialization.h"
#include "config/ConfigValidator.h"
#include "config/DefaultConfig.h"
#include "core/AtomicFile.h"
#include "logging/Logger.h"

namespace nexus::config {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::ServiceState;
using core::Status;

ConfigManager::ConfigManager(std::string path, core::EventBus* bus)
    : path_(std::move(path)), bus_(bus), config_(defaults()) {}

core::ServiceState ConfigManager::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

Status ConfigManager::start() {
  Status s = load();
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = s.ok() ? ServiceState::Running : ServiceState::Degraded;
  return s;
}

Status ConfigManager::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = ServiceState::Stopped;
  return Status::success();
}

SpeakerConfig ConfigManager::get() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

Status ConfigManager::load() {
  if (!fs::exists(path_)) {
    // First boot: seed defaults and persist.
    NX_LOG_INFO("config", "no config found; seeding defaults at " + path_);
    SpeakerConfig def = defaults();
    Status w = persist(def);
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = def;
    return w;
  }

  auto tryParse = [](const std::string& file, SpeakerConfig& out) -> Status {
    std::ifstream in(file);
    if (!in) return Status::error(ErrorCode::ConfigNotFound, "cannot open " + file);
    try {
      nlohmann::json j;
      in >> j;
      out = j.get<SpeakerConfig>();
    } catch (const std::exception& e) {
      return Status::error(ErrorCode::ConfigParseError, e.what());
    }
    return ConfigValidator::validate(out);
  };

  SpeakerConfig parsed;
  Status s = tryParse(path_, parsed);
  if (s.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = parsed;
    return Status::success();
  }

  // Primary corrupt/invalid — try the backup.
  NX_LOG_ERROR("config", s.code(), "primary config invalid: " + s.message());
  const std::string bak = path_ + ".bak";
  if (fs::exists(bak)) {
    SpeakerConfig fromBak;
    Status bs = tryParse(bak, fromBak);
    if (bs.ok()) {
      NX_LOG_WARN("config", "recovered config from backup " + bak);
      // Preserve the corrupt file for diagnostics; promote the backup.
      std::error_code ec;
      fs::rename(path_, path_ + ".corrupt", ec);
      persist(fromBak);
      std::lock_guard<std::mutex> lock(mutex_);
      config_ = fromBak;
      if (bus_) bus_->publish(core::Event{core::EventType::ConfigInvalid, "config"});
      return Status::success();
    }
  }

  // Backup also unusable — fall back to defaults, keep the corrupt file, raise event.
  NX_LOG_CRIT("config", ErrorCode::ConfigCorrupt, "config unrecoverable; using defaults");
  std::error_code ec;
  fs::rename(path_, path_ + ".corrupt", ec);
  SpeakerConfig def = defaults();
  persist(def);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = def;
  }
  if (bus_) bus_->publish(core::Event{core::EventType::ConfigInvalid, "config"});
  return Status::error(ErrorCode::ConfigCorrupt, "config was corrupt; reverted to defaults");
}

Status ConfigManager::reloadFromDisk() { return load(); }

Status ConfigManager::persist(const SpeakerConfig& cfg) {
  Status v = ConfigValidator::validate(cfg);
  if (!v.ok()) return v;  // never persist invalid config

  // Keep a one-deep backup of the current on-disk file before overwriting.
  if (fs::exists(path_)) {
    std::error_code ec;
    fs::copy_file(path_, path_ + ".bak", fs::copy_options::overwrite_existing, ec);
  }

  nlohmann::json j = cfg;
  return core::atomicWriteFile(path_, j.dump(2));
}

Status ConfigManager::update(const std::function<void(SpeakerConfig&)>& mutator) {
  SpeakerConfig candidate;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    candidate = config_;
  }
  mutator(candidate);

  Status v = ConfigValidator::validate(candidate);
  if (!v.ok()) {
    NX_LOG_WARN("config", "rejected config update: " + v.message());
    return v;  // disk + memory untouched
  }
  Status w = persist(candidate);
  if (!w.ok()) return w;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = candidate;
  }
  if (bus_) bus_->publish(core::Event{core::EventType::ConfigChanged, "config"});
  return Status::success();
}

Status ConfigManager::resetToDefaults() {
  SpeakerConfig def = defaults();
  Status w = persist(def);
  if (!w.ok()) return w;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = def;
  }
  if (bus_) bus_->publish(core::Event{core::EventType::ConfigChanged, "config"});
  return Status::success();
}

}  // namespace nexus::config
