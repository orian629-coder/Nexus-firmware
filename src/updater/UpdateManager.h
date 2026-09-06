#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/EventBus.h"
#include "core/IService.h"
#include "updater/IUpdateSource.h"
#include "updater/Installer.h"
#include "updater/RollbackManager.h"
#include "updater/SignatureValidator.h"

namespace nexus::updater {

// Orchestrates signed OTA software updates with rollback, per the spec:
//   download → verify signature → check compatibility → backup → install → restart →
//   health check → confirm / rollback.
//
// The download source, installer target, and post-install "restart + health check" are injectable
// so the whole flow is testable off-target without touching the real binary or restarting. Rules:
// no unsigned/tampered/incompatible package is installed; a failed health check restores the
// backup; updates are refused while calibrating.
class UpdateManager : public core::IService {
 public:
  // HealthCheckFn: after install, confirm the new binary is healthy (real impl: restart via
  // systemd + probe). Returns true on success.
  using HealthCheckFn = std::function<bool()>;

  UpdateManager(core::EventBus* bus, std::string vendor_public_key_b64, std::string target_path,
                std::unique_ptr<IUpdateSource> source = nullptr);

  std::string name() const override { return "updater"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  void setHealthCheck(HealthCheckFn fn) { health_check_ = std::move(fn); }
  // Gate updates during sensitive operations (e.g. calibration).
  void setUpdatesAllowed(bool allowed) { updates_allowed_ = allowed; }

  // Run the full OTA flow for the package at `location`. Emits UpdateStarted/Completed/Failed and,
  // on health-check failure, RollbackTriggered + a restore.
  core::Status runUpdate(const std::string& location,
                         const std::string& current_hardware_version = "1.0");

 private:
  core::EventBus* bus_;
  SignatureValidator validator_;
  Installer installer_;
  RollbackManager rollback_;
  std::unique_ptr<IUpdateSource> source_;
  HealthCheckFn health_check_;
  bool updates_allowed_ = true;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::updater
