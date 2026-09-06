#include <gtest/gtest.h>

#include "core/EventBus.h"
#include "diagnostics/DiagnosticService.h"

using namespace nexus::diagnostics;
using nexus::core::EventBus;

TEST(DiagnosticService, AllOkReportsOk) {
  EventBus bus;
  DiagnosticService diag(&bus);
  diag.addCheck("network", [] { return std::make_pair(CheckResult::Ok, ""); });
  diag.addCheck("audio", [] { return std::make_pair(CheckResult::Ok, ""); });
  auto report = diag.run("diag-1");
  EXPECT_EQ(report.overall, CheckResult::Ok);
  EXPECT_EQ(report.checks.size(), 2u);
}

TEST(DiagnosticService, WarningPropagatesToOverall) {
  EventBus bus;
  DiagnosticService diag(&bus);
  diag.addCheck("network", [] { return std::make_pair(CheckResult::Ok, ""); });
  diag.addCheck("microphone", [] { return std::make_pair(CheckResult::Warning, "low signal"); });
  auto report = diag.run("diag-2");
  EXPECT_EQ(report.overall, CheckResult::Warning);
}

TEST(DiagnosticService, ErrorDominates) {
  EventBus bus;
  DiagnosticService diag(&bus);
  diag.addCheck("a", [] { return std::make_pair(CheckResult::Warning, ""); });
  diag.addCheck("b", [] { return std::make_pair(CheckResult::Error, "amp fault"); });
  diag.addCheck("c", [] { return std::make_pair(CheckResult::Ok, ""); });
  auto report = diag.run("diag-3");
  EXPECT_EQ(report.overall, CheckResult::Error);
}

TEST(DiagnosticService, ThrowingProbeBecomesError) {
  EventBus bus;
  DiagnosticService diag(&bus);
  diag.addCheck("bad", []() -> std::pair<CheckResult, std::string> {
    throw std::runtime_error("boom");
  });
  auto report = diag.run("diag-4");
  EXPECT_EQ(report.overall, CheckResult::Error);
}

TEST(DiagnosticService, ReportSerializesToJson) {
  EventBus bus;
  DiagnosticService diag(&bus);
  diag.addCheck("network", [] { return std::make_pair(CheckResult::Ok, ""); });
  diag.addCheck("microphone", [] { return std::make_pair(CheckResult::Warning, ""); });
  auto j = diag.run("diag-1001").toJson();
  EXPECT_EQ(j["test_id"], "diag-1001");
  EXPECT_EQ(j["status"], "completed");
  EXPECT_EQ(j["result"], "warning");
  EXPECT_EQ(j["checks"]["network"], "ok");
  EXPECT_EQ(j["checks"]["microphone"], "warning");
}
