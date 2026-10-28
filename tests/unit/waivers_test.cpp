// Unit tests for applying human waivers to gate reports (design section
// 10.2).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cf/orch/waivers.hpp"

namespace {

using cf::gates::GateFinding;
using cf::gates::GateReport;
using cf::gates::GateResult;
using cf::store::WaiverRecord;

GateResult failed_gate(std::string gate, std::vector<std::string> messages) {
  GateResult result;
  result.gate = std::move(gate);
  result.passed = false;
  for (const auto& message : messages) {
    result.findings.push_back(GateFinding{result.gate, "error", message});
  }
  return result;
}

WaiverRecord waiver(std::string rule, std::string scope) {
  WaiverRecord record;
  record.run_id = "R12";
  record.rule = std::move(rule);
  record.scope = std::move(scope);
  record.justification = "approved";
  record.approved_by = "alice";
  return record;
}

TEST(WaiversTest, CoversEveryFindingOfTheNamedGate) {
  GateReport report;
  report.results.push_back(failed_gate(
      "guardrails", {"src/bus.cpp:42: use of banned API", "src/bus.cpp:50: cast"}));
  report.passed = false;

  cf::orch::apply_waivers(report, {waiver("guardrails", "*")});
  ASSERT_EQ(report.results.size(), 1u);
  EXPECT_TRUE(report.results[0].waived);
  EXPECT_TRUE(report.passed);
}

TEST(WaiversTest, ScopeCoversMatchingFindingsOnly) {
  GateReport report;
  report.results.push_back(failed_gate(
      "guardrails", {"src/bus.cpp:42: use of banned API", "src/registry.cpp:7: cast"}));

  cf::orch::apply_waivers(report, {waiver("guardrails", "src/bus.cpp:42")});
  EXPECT_FALSE(report.results[0].waived);
  EXPECT_FALSE(report.passed);

  GateReport scoped;
  scoped.results.push_back(
      failed_gate("guardrails", {"src/bus.cpp:42: use of banned API"}));
  cf::orch::apply_waivers(scoped, {waiver("guardrails", "src/bus.cpp:42")});
  EXPECT_TRUE(scoped.results[0].waived);
  EXPECT_TRUE(scoped.passed);
}

TEST(WaiversTest, RuleCanBeACheckNameInsideTheFinding) {
  GateReport report;
  report.results.push_back(failed_gate(
      "guardrails", {"src/bus.cpp:42: warning: bad cast [cf-swt-c-7]"}));

  cf::orch::apply_waivers(report, {waiver("cf-swt-c-7", "*")});
  EXPECT_TRUE(report.results[0].waived);
  EXPECT_TRUE(report.passed);
}

TEST(WaiversTest, SkippedAndPassedGatesAreNotWaived) {
  GateReport report;
  GateResult skipped;
  skipped.gate = "tests";
  skipped.skipped = true;
  report.results.push_back(std::move(skipped));
  GateResult passed;
  passed.gate = "build";
  passed.passed = true;
  report.results.push_back(std::move(passed));

  cf::orch::apply_waivers(
      report, {waiver("tests", "*"), waiver("build", "*")});
  EXPECT_FALSE(report.results[0].waived);
  EXPECT_FALSE(report.results[1].waived);
  EXPECT_FALSE(report.passed);
}

TEST(WaiversTest, AnUncoveredGateKeepsTheReportFailed) {
  GateReport report;
  report.results.push_back(failed_gate("build", {"compilation failed"}));
  report.results.push_back(
      failed_gate("guardrails", {"src/bus.cpp:42: banned API"}));

  cf::orch::apply_waivers(report, {waiver("guardrails", "*")});
  EXPECT_FALSE(report.results[0].waived);
  EXPECT_TRUE(report.results[1].waived);
  EXPECT_FALSE(report.passed);
}

}  // namespace
