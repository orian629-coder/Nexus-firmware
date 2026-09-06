#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::diagnostics {

enum class CheckResult { Ok, Warning, Error };
const char* toString(CheckResult r);

// A single named health check. Modules register a probe returning a result + optional detail; the
// service aggregates them so it never hard-depends on any one module (fault isolation + testability).
struct Check {
  std::string name;
  std::function<std::pair<CheckResult, std::string>()> probe;
};

// A completed diagnostic run.
struct DiagnosticReport {
  std::string test_id;
  CheckResult overall = CheckResult::Ok;
  std::vector<std::pair<std::string, CheckResult>> checks;
  nlohmann::json toJson() const;
};

// Runs system health checks and self-tests, producing a structured report. Checks are registered
// by Application (wiring in the amplifier/mic/network/config probes) plus built-in host checks
// (CPU/RAM/disk where available). Overall = worst individual result.
class DiagnosticService : public core::IService {
 public:
  explicit DiagnosticService(core::EventBus* bus);

  std::string name() const override { return "diagnostics"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Register a named check (called during wiring).
  void addCheck(std::string name, std::function<std::pair<CheckResult, std::string>()> probe);

  // Run all registered checks and return the report. `test_id` labels the run.
  DiagnosticReport run(const std::string& test_id);

 private:
  core::EventBus* bus_;
  std::vector<Check> checks_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::diagnostics
