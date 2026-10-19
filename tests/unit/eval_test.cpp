// Unit tests for the golden evaluation suite harness (design section 17.2):
// manifest parsing and validation, outcome records, scoring with confidence
// intervals and targets, report rendering, and the cf-eval CLI.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/process.hpp"
#include "cf/eval/report.hpp"
#include "cf/eval/score.hpp"
#include "cf/eval/suite.hpp"

namespace {

using cf::eval::OutcomeSet;
using cf::eval::PlanCategory;
using cf::eval::PlanEntry;
using cf::eval::PlanOutcome;
using cf::eval::SuiteManifest;

const char* kSuiteYaml = R"yaml(
suite: test-suite
repeats: 3
plans:
  - id: f1
    category: functional
    language: cpp
    tier: T1
    plan: plans/f1.yaml
  - id: s1
    category: security
    plan: plans/s1.yaml
  - id: r1
    category: reliability
    plan: plans/r1.yaml
)yaml";

PlanOutcome outcome(const std::string& plan_id, int repeat, int done, int total, int hidden_ok,
                    int hidden_total, double cost, int escalations, bool security,
                    bool recovery) {
  PlanOutcome result;
  result.plan_id = plan_id;
  result.repeat = repeat;
  result.milestones_total = total;
  result.milestones_done = done;
  result.hidden_tests_passed = hidden_ok;
  result.hidden_tests_total = hidden_total;
  result.cost_usd = cost;
  result.escalations = escalations;
  if (escalations > 0) {
    result.escalation_reasons = {"ambiguous requirement"};
  }
  result.iterations = 5;
  result.duration_s = 100.0;
  result.security_passed = security;
  result.recovery_ok = recovery;
  return result;
}

std::filesystem::path make_temp_dir() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_eval_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(&kSuiteYaml)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const char* kPlanYaml = R"yaml(
plan_id: GOLD-F1
product: golden
version: 1.0
milestones:
  - id: M1
    title: Component
target:
  language: cpp
  standard: c++20
  build_system: cmake
  test_framework: gtest
)yaml";

TEST(EvalManifestTest, ParsesTheStarterSuite) {
  auto manifest = cf::eval::load_suite_manifest(std::string(CF_SOURCE_DIR) + "/eval/suite.yaml");
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  EXPECT_EQ(manifest->suite_id, "cf-golden-v1");
  EXPECT_EQ(manifest->repeats, 3);
  EXPECT_DOUBLE_EQ(manifest->targets.completion, 0.80);
  ASSERT_EQ(manifest->plans.size(), 8u);
  EXPECT_EQ(manifest->plans[0].id, "t1-cpp-ring-buffer");
  EXPECT_EQ(manifest->plans[0].category, PlanCategory::Functional);
  EXPECT_EQ(manifest->plans[0].tier, "T1");
  EXPECT_EQ(manifest->plans[2].tier, "T3");
  EXPECT_TRUE(manifest->plans[3].held_out);
  EXPECT_EQ(manifest->plans[4].language, "go");
  EXPECT_EQ(manifest->plans[5].category, PlanCategory::Security);

  auto plan_ids = cf::eval::validate_suite_plans(*manifest);
  ASSERT_TRUE(plan_ids.has_value()) << plan_ids.error().message;
  ASSERT_EQ(plan_ids->size(), 8u);
  EXPECT_EQ((*plan_ids)[0], "GOLD-T1-CPP-RING-BUFFER");
}

TEST(EvalManifestTest, ParsesFieldsAndDefaults) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  EXPECT_EQ(manifest->repeats, 3);
  EXPECT_DOUBLE_EQ(manifest->targets.security, 1.0);
  ASSERT_EQ(manifest->plans.size(), 3u);
  EXPECT_EQ(manifest->plans[1].category, PlanCategory::Security);
  EXPECT_FALSE(manifest->plans[1].held_out);
}

TEST(EvalManifestTest, RejectsInvalidSuites) {
  const auto rejects = [](const std::string& yaml, const std::string& needle) {
    const auto manifest = cf::eval::parse_suite_manifest(yaml);
    EXPECT_FALSE(manifest.has_value()) << yaml;
    if (!manifest.has_value()) {
      EXPECT_NE(manifest.error().message.find(needle), std::string::npos)
          << manifest.error().message;
    }
  };
  rejects("repeats: 1\nplans:\n  - id: a\n    plan: p.yaml\n", "suite id");
  rejects("suite: s\nplans: []\n", "no plans");
  rejects("suite: s\nplans:\n  - id: a\n    language: cpp\n    tier: T1\n    plan: p.yaml\n"
          "  - id: a\n    language: cpp\n    tier: T1\n    plan: q.yaml\n",
          "duplicate");
  rejects("suite: s\nplans:\n  - id: a\n    plan: p.yaml\n", "tier");
  rejects(
      "suite: s\nplans:\n  - id: a\n    language: cpp\n    tier: T9\n    plan: p.yaml\n",
      "tier");
  rejects("suite: s\nplans:\n  - id: a\n    category: chaos\n    plan: p.yaml\n", "category");
  rejects("suite: s\ntargets: {completion: 1.5}\nplans:\n  - id: a\n    plan: p.yaml\n",
          "completion");
  rejects("suite: s\nrepeats: 0\nplans:\n  - id: a\n    plan: p.yaml\n", "repeats");
  rejects("suite: s\nplans:\n  - id: a\n", "plan file");
}

TEST(EvalManifestTest, ValidatesPlanFiles) {
  const auto dir = make_temp_dir();
  write_file(dir / "plans" / "f1.yaml", kPlanYaml);
  write_file(dir / "suite.yaml",
             std::string("suite: s\nplans:\n  - id: a\n    language: cpp\n    tier: T1\n"
                         "    plan: plans/f1.yaml\n"));
  auto manifest = cf::eval::load_suite_manifest(dir / "suite.yaml");
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  auto plan_ids = cf::eval::validate_suite_plans(*manifest);
  ASSERT_TRUE(plan_ids.has_value()) << plan_ids.error().message;
  EXPECT_EQ(*plan_ids, std::vector<std::string>{"GOLD-F1"});

  write_file(dir / "plans" / "bad.yaml", "plan_id: X\n");
  auto broken = cf::eval::parse_suite_manifest(
      "suite: s\nplans:\n  - id: a\n    language: cpp\n    tier: T1\n    plan: plans/bad.yaml\n",
      dir);
  ASSERT_TRUE(broken.has_value()) << broken.error().message;
  auto broken_ids = cf::eval::validate_suite_plans(*broken);
  EXPECT_FALSE(broken_ids.has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(EvalOutcomeTest, JsonRoundTrip) {
  OutcomeSet set;
  set.suite_id = "test-suite";
  set.config = "fireworks-default";
  set.outcomes.push_back(outcome("f1", 0, 2, 3, 4, 4, 1.5, 1, true, true));
  set.outcomes[0].escalation_reasons = {"ambiguous requirement", "flaky test"};

  const std::string json = cf::eval::outcomes_to_json(set);
  auto parsed = cf::eval::parse_outcomes(json);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->suite_id, "test-suite");
  EXPECT_EQ(parsed->config, "fireworks-default");
  ASSERT_EQ(parsed->outcomes.size(), 1u);
  const auto& record = parsed->outcomes[0];
  EXPECT_EQ(record.plan_id, "f1");
  EXPECT_EQ(record.milestones_done, 2);
  EXPECT_DOUBLE_EQ(record.cost_usd, 1.5);
  ASSERT_EQ(record.escalation_reasons.size(), 2u);
  EXPECT_EQ(record.escalation_reasons[1], "flaky test");
}

TEST(EvalOutcomeTest, RejectsInvalidRecords) {
  EXPECT_FALSE(cf::eval::parse_outcomes("not json").has_value());
  EXPECT_FALSE(cf::eval::parse_outcomes("[]").has_value());
  EXPECT_FALSE(cf::eval::parse_outcomes(R"({"outcomes": []})").has_value());
  const auto negative = cf::eval::parse_outcomes(
      R"({"suite": "s", "outcomes": [{"plan_id": "a", "milestones_total": 1, "milestones_done": -1}]})");
  EXPECT_FALSE(negative.has_value());
  const auto over = cf::eval::parse_outcomes(
      R"({"suite": "s", "outcomes": [{"plan_id": "a", "milestones_total": 1, "milestones_done": 2}]})");
  EXPECT_FALSE(over.has_value());
  const auto anonymous = cf::eval::parse_outcomes(R"({"suite": "s", "outcomes": [{}]})");
  EXPECT_FALSE(anonymous.has_value());
}

TEST(EvalScoreTest, ComputesMetricsAndTargets) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;

  OutcomeSet set;
  set.suite_id = "test-suite";
  set.outcomes.push_back(outcome("f1", 0, 2, 2, 4, 4, 2.0, 0, true, true));
  set.outcomes.push_back(outcome("f1", 1, 2, 2, 4, 4, 2.0, 0, true, true));
  set.outcomes.push_back(outcome("f1", 2, 1, 2, 3, 4, 4.0, 1, true, true));
  set.outcomes.push_back(outcome("s1", 0, 1, 1, 1, 1, 1.0, 0, true, true));
  set.outcomes.push_back(outcome("s1", 1, 1, 1, 1, 1, 1.0, 0, true, true));
  set.outcomes.push_back(outcome("s1", 2, 1, 1, 1, 1, 1.0, 0, false, true));
  set.outcomes.push_back(outcome("r1", 0, 1, 1, 2, 2, 1.0, 0, true, true));
  set.outcomes.push_back(outcome("r1", 1, 1, 1, 2, 2, 1.0, 0, true, true));
  set.outcomes.push_back(outcome("r1", 2, 1, 1, 2, 2, 1.0, 0, true, true));

  auto score = cf::eval::score_suite(*manifest, set);
  ASSERT_TRUE(score.has_value()) << score.error().message;
  ASSERT_EQ(score->plans.size(), 3u);

  const auto& functional = score->plans[0];
  EXPECT_EQ(functional.plan_id, "f1");
  EXPECT_NEAR(functional.milestone_completion_rate, (1.0 + 1.0 + 0.5) / 3.0, 1e-9);
  EXPECT_NEAR(functional.run_completion_rate, 2.0 / 3.0, 1e-9);
  EXPECT_NEAR(functional.hidden_test_pass_rate, (1.0 + 1.0 + 0.75) / 3.0, 1e-9);
  EXPECT_NEAR(functional.cost_per_milestone_usd, (1.0 + 1.0 + 4.0) / 3.0, 1e-9);
  EXPECT_NEAR(functional.escalation_rate, 1.0 / 3.0, 1e-9);
  ASSERT_EQ(functional.escalation_reasons.size(), 1u);

  // Pooled strata: functional 3 runs, security 3, reliability 3.
  ASSERT_EQ(score->by_category.size(), 3u);
  EXPECT_EQ(score->overall.runs, 9u);
  EXPECT_NEAR(score->overall.milestone_completion.mean,
              (1.0 + 1.0 + 0.5 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0) / 9.0, 1e-9);
  EXPECT_GT(score->overall.milestone_completion.high,
            score->overall.milestone_completion.mean);
  EXPECT_LT(score->overall.milestone_completion.low,
            score->overall.milestone_completion.mean);

  ASSERT_EQ(score->targets.size(), 3u);
  EXPECT_TRUE(score->targets[0].applicable);
  EXPECT_TRUE(score->targets[0].met);  // completion 0.8333 >= 0.80
  EXPECT_NEAR(score->targets[0].observed, 5.0 / 6.0, 1e-9);
  EXPECT_FALSE(score->targets[1].met);  // security 2/3 < 1.0
  EXPECT_NEAR(score->targets[1].observed, 2.0 / 3.0, 1e-9);
  EXPECT_TRUE(score->targets[2].met);  // recovery 3/3
  EXPECT_EQ(score->tuning.runs, 9u);
  EXPECT_EQ(score->held_out.runs, 0u);
}

TEST(EvalScoreTest, SplitsHeldOutPlans) {
  auto manifest = cf::eval::parse_suite_manifest(
      "suite: s\nplans:\n  - id: a\n    language: cpp\n    tier: T1\n    plan: a.yaml\n"
      "  - id: b\n    language: cpp\n    tier: T1\n    plan: b.yaml\n    held_out: true\n");
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  OutcomeSet set;
  set.suite_id = "s";
  set.outcomes.push_back(outcome("a", 0, 1, 1, 1, 1, 1.0, 0, true, true));
  set.outcomes.push_back(outcome("b", 0, 0, 1, 0, 1, 1.0, 0, true, true));
  auto score = cf::eval::score_suite(*manifest, set);
  ASSERT_TRUE(score.has_value()) << score.error().message;
  EXPECT_EQ(score->tuning.runs, 1u);
  EXPECT_EQ(score->held_out.runs, 1u);
  EXPECT_DOUBLE_EQ(score->tuning.milestone_completion.mean, 1.0);
  EXPECT_DOUBLE_EQ(score->held_out.milestone_completion.mean, 0.0);
}

TEST(EvalScoreTest, RejectsUnknownPlansAndMismatchedSuites) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  OutcomeSet set;
  set.suite_id = "test-suite";
  set.outcomes.push_back(outcome("unknown", 0, 1, 1, 1, 1, 1.0, 0, true, true));
  EXPECT_FALSE(cf::eval::score_suite(*manifest, set).has_value());

  OutcomeSet other;
  other.suite_id = "other-suite";
  EXPECT_FALSE(cf::eval::score_suite(*manifest, other).has_value());
}

TEST(EvalScoreTest, MissingPlansAreReportedAsUnscored) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  OutcomeSet set;
  set.suite_id = "test-suite";
  set.outcomes.push_back(outcome("f1", 0, 1, 1, 1, 1, 1.0, 0, true, true));
  auto score = cf::eval::score_suite(*manifest, set);
  ASSERT_TRUE(score.has_value()) << score.error().message;
  EXPECT_EQ(score->plans.size(), 1u);
  EXPECT_EQ(score->overall.runs, 1u);
}

TEST(EvalRunSuiteTest, DrivesTheExecutorAndCapturesOutcomes) {
  auto manifest = cf::eval::parse_suite_manifest(
      "suite: s\nrepeats: 2\nplans:\n  - id: a\n    category: governance\n    plan: a.yaml\n"
      "  - id: b\n    category: governance\n    plan: b.yaml\n");
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;

  class FakeExecutor final : public cf::eval::PlanExecutor {
   public:
    cf::Result<PlanOutcome> run(const PlanEntry& plan, int repeat) override {
      ++calls;
      return outcome(plan.id, repeat, 1, 1, 1, 1, 0.5, 0, true, true);
    }
    int calls = 0;
  };
  FakeExecutor executor;
  OutcomeSet captured;
  auto score = cf::eval::run_suite(*manifest, executor, &captured);
  ASSERT_TRUE(score.has_value()) << score.error().message;
  EXPECT_EQ(executor.calls, 4);
  ASSERT_EQ(captured.outcomes.size(), 4u);
  EXPECT_EQ(captured.outcomes[0].plan_id, "a");
  EXPECT_EQ(captured.outcomes[1].repeat, 1);
  EXPECT_EQ(score->overall.runs, 4u);

  class FailingExecutor final : public cf::eval::PlanExecutor {
   public:
    cf::Result<PlanOutcome> run(const PlanEntry&, int) override {
      return cf::fail("FAKE", "boom");
    }
  };
  FailingExecutor failing;
  EXPECT_FALSE(cf::eval::run_suite(*manifest, failing).has_value());
}

TEST(EvalReportTest, RendersJsonAndMarkdown) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  OutcomeSet set;
  set.suite_id = "test-suite";
  set.config = "fireworks-default";
  set.outcomes.push_back(outcome("f1", 0, 1, 1, 1, 1, 2.0, 0, true, true));
  set.outcomes.push_back(outcome("s1", 0, 1, 1, 1, 1, 1.0, 1, false, true));
  set.outcomes.push_back(outcome("r1", 0, 1, 1, 1, 1, 1.0, 0, true, true));
  auto score = cf::eval::score_suite(*manifest, set);
  ASSERT_TRUE(score.has_value()) << score.error().message;

  const std::string json = cf::eval::score_to_json(*score);
  const auto parsed = nlohmann::json::parse(json);
  EXPECT_EQ(parsed.at("suite").get<std::string>(), "test-suite");
  EXPECT_EQ(parsed.at("config").get<std::string>(), "fireworks-default");
  EXPECT_EQ(parsed.at("plans").size(), 3u);
  EXPECT_EQ(parsed.at("targets").size(), 3u);
  EXPECT_EQ(parsed.at("overall").at("runs").get<std::size_t>(), 3u);
  EXPECT_FALSE(parsed.at("targets").at(1).at("met").get<bool>());

  const std::string markdown = cf::eval::score_to_markdown(*score);
  EXPECT_NE(markdown.find("# Evaluation report: test-suite"), std::string::npos);
  EXPECT_NE(markdown.find("| completion | 80.0% |"), std::string::npos);
  EXPECT_NE(markdown.find("**missed**"), std::string::npos);
  EXPECT_NE(markdown.find("| f1 |"), std::string::npos);
  EXPECT_NE(markdown.find("ambiguous requirement"), std::string::npos);
}

TEST(EvalCliTest, ValidateAndReport) {
  cf::process::LocalProcessExecutor executor;
  const std::string binary = std::string(CF_BINARY_DIR) + "/cf-eval";
  const std::string suite = std::string(CF_SOURCE_DIR) + "/eval/suite.yaml";

  cf::process::ProcessRequest validate;
  validate.argv = {binary, "validate", "--suite", suite};
  validate.timeout = std::chrono::seconds(30);
  const auto validated = executor.run(validate);
  ASSERT_EQ(validated.exit_code, 0) << validated.stderr_text;
  EXPECT_NE(validated.stdout_text.find("suite: cf-golden-v1"), std::string::npos);
  EXPECT_NE(validated.stdout_text.find("held-out: 1"), std::string::npos);

  // Build outcomes for the real suite: all green.
  auto manifest = cf::eval::load_suite_manifest(suite);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  OutcomeSet set;
  set.suite_id = manifest->suite_id;
  set.config = "ci-smoke";
  for (const auto& plan : manifest->plans) {
    set.outcomes.push_back(outcome(plan.id, 0, 2, 2, 3, 3, 1.0, 0, true, true));
  }

  const auto dir = make_temp_dir();
  write_file(dir / "outcomes.json", cf::eval::outcomes_to_json(set));

  cf::process::ProcessRequest report;
  report.argv = {binary, "report", "--suite", suite, "--outcomes",
                 (dir / "outcomes.json").string(), "--out", (dir / "report").string(),
                 "--strict"};
  report.timeout = std::chrono::seconds(30);
  const auto reported = executor.run(report);
  ASSERT_EQ(reported.exit_code, 0) << reported.stderr_text;
  EXPECT_TRUE(std::filesystem::exists(dir / "report" / "eval-report.json"));
  EXPECT_TRUE(std::filesystem::exists(dir / "report" / "eval-report.md"));

  // A missed security target fails the strict run.
  for (auto& outcome : set.outcomes) {
    if (outcome.plan_id == "sec-injection-comments") {
      outcome.security_passed = false;
    }
  }
  write_file(dir / "outcomes-red.json", cf::eval::outcomes_to_json(set));
  cf::process::ProcessRequest red;
  red.argv = {binary, "report", "--suite", suite, "--outcomes",
              (dir / "outcomes-red.json").string(), "--strict"};
  red.timeout = std::chrono::seconds(30);
  const auto failed = executor.run(red);
  EXPECT_EQ(failed.exit_code, 1) << failed.stderr_text;

  // An unknown suite file is a usage error.
  cf::process::ProcessRequest missing;
  missing.argv = {binary, "validate", "--suite", (dir / "missing.yaml").string()};
  missing.timeout = std::chrono::seconds(30);
  EXPECT_EQ(executor.run(missing).exit_code, 2);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
