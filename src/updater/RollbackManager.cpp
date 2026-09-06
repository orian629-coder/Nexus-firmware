#include "updater/RollbackManager.h"

#include <filesystem>

namespace nexus::updater {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::Status;

Status RollbackManager::backup() {
  std::error_code ec;
  if (!fs::exists(target_)) return Status::error(ErrorCode::NotFound, "target missing: " + target_);
  fs::copy_file(target_, backup_, fs::copy_options::overwrite_existing, ec);
  if (ec) return Status::error(ErrorCode::IoError, "backup failed: " + ec.message());
  return Status::success();
}

Status RollbackManager::restore() {
  std::error_code ec;
  if (!fs::exists(backup_)) return Status::error(ErrorCode::NotFound, "no backup to restore");
  fs::copy_file(backup_, target_, fs::copy_options::overwrite_existing, ec);
  if (ec) return Status::error(ErrorCode::IoError, "restore failed: " + ec.message());
  return Status::success();
}

bool RollbackManager::hasBackup() const { return fs::exists(backup_); }

}  // namespace nexus::updater
