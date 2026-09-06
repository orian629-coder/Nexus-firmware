#include "updater/UpdateManager.h"

#include "logging/Logger.h"

#if NEXUS_STUB_HAL
// StubUpdateSource is defined in IUpdateSource.h.
#endif

namespace nexus::updater {

using core::ErrorCode;
using core::Event;
using core::EventType;
using core::ServiceState;
using core::Status;

UpdateManager::UpdateManager(core::EventBus* bus, std::string vendor_public_key_b64,
                             std::string target_path, std::unique_ptr<IUpdateSource> source)
    : bus_(bus),
      validator_(std::move(vendor_public_key_b64)),
      installer_(target_path),
      rollback_(target_path),
      source_(source ? std::move(source) : std::make_unique<StubUpdateSource>()) {}

Status UpdateManager::start() {
  state_ = ServiceState::Running;
  return Status::success();
}

Status UpdateManager::stop() {
  state_ = ServiceState::Stopped;
  return Status::success();
}

Status UpdateManager::runUpdate(const std::string& location,
                                const std::string& current_hardware_version) {
  if (!updates_allowed_) {
    return Status::error(ErrorCode::PermissionDenied, "updates not allowed right now");
  }

  auto fail = [&](Status s) -> Status {
    NX_LOG_ERROR("updater", s.code(), "update failed: " + s.message());
    if (bus_) bus_->publish(Event{EventType::UpdateFailed, "updater", {{"error", s.message()}}});
    return s;
  };

  if (bus_) bus_->publish(Event{EventType::UpdateStarted, "updater"});
  NX_LOG_INFO("updater", "starting update from " + location);

  // 1. Download.
  auto pkg = source_->fetch(location);
  if (!pkg.ok()) return fail(pkg.status());

  // 2. Verify signature + payload hash.
  Status v = validator_.verify(pkg.value());
  if (!v.ok()) return fail(v);

  // 3. Compatibility.
  const auto& manifest = pkg.value().manifest;
  if (!manifest.min_hardware_version.empty() &&
      current_hardware_version < manifest.min_hardware_version) {
    return fail(Status::error(ErrorCode::InvalidArg,
                              "hardware " + current_hardware_version + " < required " +
                                  manifest.min_hardware_version));
  }

  // 4. Backup the current binary.
  Status b = rollback_.backup();
  if (!b.ok()) return fail(b);

  // 5. Install the new payload.
  Status i = installer_.install(pkg.value().payload);
  if (!i.ok()) return fail(i);

  // 6-8. Restart + health check. On failure, roll back to the backup.
  const bool healthy = health_check_ ? health_check_() : true;
  if (!healthy) {
    NX_LOG_CRIT("updater", ErrorCode::ServiceStartFailed,
                "post-update health check failed; rolling back");
    if (bus_) bus_->publish(Event{EventType::RollbackTriggered, "updater"});
    Status r = rollback_.restore();
    if (!r.ok()) {
      return fail(Status::error(ErrorCode::IoError, "rollback failed: " + r.message()));
    }
    return fail(Status::error(ErrorCode::ServiceStartFailed, "health check failed, rolled back"));
  }

  // 9. Success.
  NX_LOG_INFO("updater", "update to " + manifest.version + " completed");
  if (bus_) {
    bus_->publish(Event{EventType::UpdateCompleted, "updater", {{"version", manifest.version}}});
  }
  return Status::success();
}

}  // namespace nexus::updater
