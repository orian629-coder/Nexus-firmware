#include "config/StreamerConfigManager.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "config/StreamerConfigSerialization.h"
#include "core/AtomicFile.h"

namespace nexus::streamer::config {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::ServiceState;
using core::Status;

StreamerConfigManager::StreamerConfigManager(std::string path) : path_(std::move(path)) {}

Status StreamerConfigManager::start() {
  std::lock_guard<std::mutex> lk(mutex_);
  state_ = ServiceState::Starting;

  std::error_code ec;
  if (!fs::exists(path_, ec)) {
    // First run: seed defaults so the file exists and is inspectable.
    config_ = StreamerConfig{};
    const Status s = saveLocked();
    state_ = s.ok() ? ServiceState::Running : ServiceState::Degraded;
    return s;
  }

  std::ifstream in(path_);
  std::stringstream buf;
  buf << in.rdbuf();

  auto parse = [&](const std::string& text, StreamerConfig& out) -> bool {
    auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    try {
      out = j.get<StreamerConfig>();
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };

  StreamerConfig loaded;
  if (parse(buf.str(), loaded)) {
    config_ = std::move(loaded);
    state_ = ServiceState::Running;
    return Status::success();
  }

  // Corrupt primary. Try the backup before giving up — losing the speaker list to a truncated write
  // would mean re-pairing every speaker by hand.
  const std::string bak = path_ + ".bak";
  if (fs::exists(bak, ec)) {
    std::ifstream bin(bak);
    std::stringstream bbuf;
    bbuf << bin.rdbuf();
    StreamerConfig recovered;
    if (parse(bbuf.str(), recovered)) {
      // Keep the damaged file for inspection rather than silently overwriting the evidence.
      fs::rename(path_, path_ + ".corrupt", ec);
      config_ = std::move(recovered);
      const Status s = saveLocked();
      state_ = ServiceState::Running;
      return s.ok() ? Status::success() : s;
    }
  }

  fs::rename(path_, path_ + ".corrupt", ec);
  config_ = StreamerConfig{};
  saveLocked();
  state_ = ServiceState::Degraded;
  return Status::error(ErrorCode::Corrupt, "streamer config unreadable; reset to defaults");
}

Status StreamerConfigManager::stop() {
  state_ = ServiceState::Stopped;
  return Status::success();
}

StreamerConfig StreamerConfigManager::get() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return config_;
}

Status StreamerConfigManager::update(const std::function<void(StreamerConfig&)>& mutator) {
  std::lock_guard<std::mutex> lk(mutex_);
  mutator(config_);
  return saveLocked();
}

Status StreamerConfigManager::saveLocked() const {
  nlohmann::json j = config_;
  const std::string text = j.dump(2);

  // Roll the previous good file to .bak first, so a failure mid-write still leaves one readable
  // copy on disk.
  std::error_code ec;
  if (fs::exists(path_, ec)) {
    fs::copy_file(path_, path_ + ".bak", fs::copy_options::overwrite_existing, ec);
  }
  return core::atomicWriteFile(path_, text);
}

}  // namespace nexus::streamer::config
