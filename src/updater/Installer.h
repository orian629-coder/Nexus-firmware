#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Result.h"

namespace nexus::updater {

// Installs the verified update payload by atomically replacing the target binary (temp write +
// fsync + rename), preserving the executable mode. The actual process restart is orchestrated by
// UpdateManager (systemd restarts the service), not here.
class Installer {
 public:
  explicit Installer(std::string target_path) : target_(std::move(target_path)) {}

  core::Status install(const std::vector<std::uint8_t>& payload);

 private:
  std::string target_;
};

}  // namespace nexus::updater
