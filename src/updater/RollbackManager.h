#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::updater {

// Keeps a backup of the current binary so a failed update can be rolled back. The backup is a copy
// of the target file at `<target>.prev`. Restoring copies it back over the target.
class RollbackManager {
 public:
  explicit RollbackManager(std::string target_path) : target_(std::move(target_path)) {}

  const std::string& backupPath() const { return backup_; }

  core::Status backup();    // copy target -> target.prev
  core::Status restore();   // copy target.prev -> target
  bool hasBackup() const;

 private:
  std::string target_;
  std::string backup_ = target_ + ".prev";
};

}  // namespace nexus::updater
