#include "diagnostics/DiagnosticService.h"

#include "logging/Logger.h"

namespace nexus::diagnostics {

using core::ServiceState;
using core::Status;

const char* toString(CheckResult r) {
  switch (r) {
    case CheckResult::Ok: return "ok";
    case CheckResult::Warning: return "warning";
    case CheckResult::Error: return "error";
  }
  return "unknown";
}

nlohmann::json DiagnosticReport::toJson() const {
  nlohmann::json checks_json = nlohmann::json::object();
  for (const auto& [n, r] : checks) checks_json[n] = toString(r);
  return {{"test_id", test_id},
          {"status", "completed"},
          {"result", toString(overall)},
          {"checks", checks_json}};
}

DiagnosticService::DiagnosticService(core::EventBus* bus) : bus_(bus) {}

Status DiagnosticService::start() {
  state_ = ServiceState::Running;
  return Status::success();
}

Status DiagnosticService::stop() {
  state_ = ServiceState::Stopped;
  return Status::success();
}

void DiagnosticService::addCheck(std::string name,
                                 std::function<std::pair<CheckResult, std::string>()> probe) {
  checks_.push_back({std::move(name), std::move(probe)});
}

DiagnosticReport DiagnosticService::run(const std::string& test_id) {
  DiagnosticReport report;
  report.test_id = test_id;
  report.overall = CheckResult::Ok;

  for (const auto& check : checks_) {
    CheckResult r = CheckResult::Ok;
    std::string detail;
    try {
      auto out = check.probe();
      r = out.first;
      detail = out.second;
    } catch (const std::exception& e) {
      r = CheckResult::Error;
      detail = e.what();
    }
    report.checks.emplace_back(check.name, r);
    if (r == CheckResult::Error) {
      report.overall = CheckResult::Error;
    } else if (r == CheckResult::Warning && report.overall == CheckResult::Ok) {
      report.overall = CheckResult::Warning;
    }
    if (!detail.empty()) {
      NX_LOG_INFO("diagnostics", check.name + ": " + std::string(toString(r)) + " (" + detail + ")");
    }
  }

  NX_LOG_INFO("diagnostics", "diagnostic run " + test_id + " -> " + toString(report.overall));
  return report;
}

}  // namespace nexus::diagnostics
