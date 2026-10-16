// Unit tests for the model benchmark harness (design sections 17.2, 18.2
// Phase 0b): benchmark configuration parsing, suite restriction, candidate
// recommendation, report rendering, hidden-test specs and the hidden-test
// runner.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/eval/benchmark.hpp"
#include "cf/eval/hidden.hpp"

namespace {

using cf::eval::BenchmarkConfig;
using cf::eval::Candidate;
using cf::eval::CandidateResult;
using cf::eval::HiddenTestSpec;
using cf::eval::PlanEntry;
using cf::eval::SuiteManifest;

const char* kBenchmarkYaml = R"yaml(
benchmark: cf-bench-test
provider_base_url: https://api.fireworks.ai/inference/v1
api_key_env: FIREWORKS_API_KEY
repeats: 1
plans: [f1]
candidates:
  - name: default
    roles:
      coder: accounts/fireworks/models/deepseek-v4p1-flash
      reviewer: accounts/fireworks/models/kimi-k3
      qa: accounts/fireworks/models/glm-5p3
  - name: qa-flash
    roles:
      coder: accounts/fireworks/models/deepseek-v4p1-flash
      reviewer: accounts/fireworks/models/kimi-k3
      qa: accounts/fireworks/models/glm-5p3-flash
prices:
  accounts/fireworks/models/deepseek-v4p1-flash:
    in: 0.15
    out: 0.60
)yaml";

const char* kSuiteYaml = R"yaml(
suite: test-suite
repeats: 3
plans:
  - id: f1
    category: functional
    language: cpp
    tier: T1
    plan: plans/f1.yaml
  - id: f2
    category: functional
    language: cpp
    tier: T2
    plan: plans/f2.yaml
  - id: s1
    category: security
    plan: plans/s1.yaml
)yaml";

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_eval_benchmark_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

cf::eval::PlanOutcome outcome(const std::string& plan_id, int done, int total,
                              int hidden_ok, int hidden_total, double cost) {
  cf::eval::PlanOutcome result;
  result.plan_id = plan_id;
  result.milestones_total = total;
  result.milestones_done = done;
  result.hidden_tests_passed = hidden_ok;
  result.hidden_tests_total = hidden_total;
  result.cost_usd = cost;
  return result;
}

CandidateResult result_with(const std::string& name, const SuiteManifest& manifest,
                            const std::string& plan_id, int done, int total,
                            int hidden_ok, int hidden_total, double cost) {
  CandidateResult result;
  result.name = name;
  result.outcomes.suite_id = manifest.suite_id;
  result.outcomes.config = name;
  result.outcomes.outcomes.push_back(
      outcome(plan_id, done, total, hidden_ok, hidden_total, cost));
  auto score = cf::eval::score_suite(manifest, result.outcomes);
  EXPECT_TRUE(score.has_value());
  if (score.has_value()) {
    result.score = *score;
  }
  return result;
}

TEST(EvalBenchmarkTest, ParsesAValidConfig) {
  auto config = cf::eval::parse_benchmark_config(kBenchmarkYaml);
  ASSERT_TRUE(config.has_value()) << config.error().message;
  EXPECT_EQ(config->benchmark_id, "cf-bench-test");
  EXPECT_EQ(config->repeats, 1);
  EXPECT_EQ(config->plans, std::vector<std::string>{"f1"});
  ASSERT_EQ(config->candidates.size(), 2u);
  EXPECT_EQ(config->candidates[0].name, "default");
  EXPECT_EQ(config->candidates[0].role_models.at("qa"),
            "accounts/fireworks/models/glm-5p3");
  EXPECT_EQ(config->candidates[1].role_models.at("qa"),
            "accounts/fireworks/models/glm-5p3-flash");
  EXPECT_DOUBLE_EQ(config->prices.at("accounts/fireworks/models/deepseek-v4p1-flash")
                       .usd_per_mtok_in,
                   0.15);
}

TEST(EvalBenchmarkTest, RejectsInvalidConfigs) {
  auto no_candidates = cf::eval::parse_benchmark_config("benchmark: b\ncandidates: []\n");
  EXPECT_FALSE(no_candidates.has_value());

  auto duplicate =
      cf::eval::parse_benchmark_config(R"yaml(
benchmark: b
candidates:
  - name: x
    roles: {coder: m}
  - name: x
    roles: {coder: m}
)yaml");
  EXPECT_FALSE(duplicate.has_value());

  auto empty_model = cf::eval::parse_benchmark_config(R"yaml(
benchmark: b
candidates:
  - name: x
    roles: {coder: ""}
)yaml");
  EXPECT_FALSE(empty_model.has_value());

  auto not_a_map = cf::eval::parse_benchmark_config("- 1\n- 2\n");
  EXPECT_FALSE(not_a_map.has_value());
}

TEST(EvalBenchmarkTest, RestrictsTheSuiteToTheBenchmarkPlans) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  auto config = cf::eval::parse_benchmark_config(kBenchmarkYaml);
  ASSERT_TRUE(config.has_value());

  auto restricted = cf::eval::restrict_suite(*manifest, *config);
  ASSERT_TRUE(restricted.has_value()) << restricted.error().message;
  ASSERT_EQ(restricted->plans.size(), 1u);
  EXPECT_EQ(restricted->plans[0].id, "f1");
  EXPECT_EQ(restricted->repeats, 1);

  // Without a plans list the whole suite runs; without repeats the suite's
  // repeat count is kept.
  auto all = cf::eval::parse_benchmark_config(R"yaml(
benchmark: b
candidates:
  - name: x
    roles: {coder: m}
)yaml");
  ASSERT_TRUE(all.has_value());
  auto unrestricted = cf::eval::restrict_suite(*manifest, *all);
  ASSERT_TRUE(unrestricted.has_value());
  EXPECT_EQ(unrestricted->plans.size(), 3u);
  EXPECT_EQ(unrestricted->repeats, 3);

  auto unknown = cf::eval::parse_benchmark_config(R"yaml(
benchmark: b
plans: [nope]
candidates:
  - name: x
    roles: {coder: m}
)yaml");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_FALSE(cf::eval::restrict_suite(*manifest, *unknown).has_value());
}

TEST(EvalBenchmarkTest, RecommendsByCompletionThenCost) {
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value());

  std::vector<CandidateResult> results;
  results.push_back(result_with("low-completion", *manifest, "f1", 1, 2, 1, 2, 1.0));
  results.push_back(result_with("high-completion", *manifest, "f1", 2, 2, 2, 2, 5.0));
  auto winner = cf::eval::recommend_candidate(results);
  ASSERT_TRUE(winner.has_value());
  EXPECT_EQ(results[*winner].name, "high-completion");

  // Equal completion: the cheaper candidate wins.
  std::vector<CandidateResult> tied;
  tied.push_back(result_with("expensive", *manifest, "f1", 2, 2, 2, 2, 5.0));
  tied.push_back(result_with("cheap", *manifest, "f1", 2, 2, 2, 2, 1.0));
  winner = cf::eval::recommend_candidate(tied);
  ASSERT_TRUE(winner.has_value());
  EXPECT_EQ(tied[*winner].name, "cheap");

  // A tie on every metric keeps the earlier candidate.
  std::vector<CandidateResult> exact;
  exact.push_back(result_with("first", *manifest, "f1", 2, 2, 2, 2, 2.0));
  exact.push_back(result_with("second", *manifest, "f1", 2, 2, 2, 2, 2.0));
  winner = cf::eval::recommend_candidate(exact);
  ASSERT_TRUE(winner.has_value());
  EXPECT_EQ(exact[*winner].name, "first");

  EXPECT_FALSE(cf::eval::recommend_candidate({}).has_value());
}

TEST(EvalBenchmarkTest, RendersTheSummary) {
  auto config = cf::eval::parse_benchmark_config(kBenchmarkYaml);
  ASSERT_TRUE(config.has_value());
  auto manifest = cf::eval::parse_suite_manifest(kSuiteYaml);
  ASSERT_TRUE(manifest.has_value());

  std::vector<CandidateResult> results;
  results.push_back(result_with("default", *manifest, "f1", 2, 2, 2, 2, 1.0));
  const auto winner = cf::eval::recommend_candidate(results);

  const std::string json =
      cf::eval::benchmark_summary_to_json(*config, results, winner);
  EXPECT_NE(json.find("\"recommended\": \"default\""), std::string::npos);

  const std::string markdown =
      cf::eval::benchmark_summary_to_markdown(*config, results, winner);
  EXPECT_NE(markdown.find("Recommended: **default**"), std::string::npos);
  EXPECT_NE(markdown.find("qa=accounts/fireworks/models/glm-5p3"),
            std::string::npos);
}

TEST(EvalHiddenTest, ParsesASpec) {
  auto spec = cf::eval::parse_hidden_test_spec(R"yaml(
security: true
compile:
  - name: build
    argv: [g++, -std=c++20, "{hidden}/test.cpp", -o, "{build}/test"]
tests:
  - name: semantics
    argv: ["{build}/test"]
  - ["{build}/test", --strict]
)yaml");
  ASSERT_TRUE(spec.has_value()) << spec.error().message;
  EXPECT_TRUE(spec->security);
  EXPECT_FALSE(spec->recovery);
  ASSERT_EQ(spec->compile.size(), 1u);
  EXPECT_EQ(spec->compile[0].name, "build");
  ASSERT_EQ(spec->tests.size(), 2u);
  EXPECT_EQ(spec->tests[1].argv.size(), 2u);

  auto no_tests = cf::eval::parse_hidden_test_spec("compile: []\n");
  EXPECT_FALSE(no_tests.has_value());
  auto bad_step = cf::eval::parse_hidden_test_spec("tests: [\"\"]\n");
  EXPECT_FALSE(bad_step.has_value());
}

TEST(EvalHiddenTest, ExpandsPlaceholders) {
  const auto expanded = cf::eval::expand_hidden_argv(
      {"{hidden}/t.cpp", "-I", "{worktree}/include", "-o", "{build}/t"},
      "/w/tree", "/h/dir", "/b/dir");
  EXPECT_EQ(expanded, (std::vector<std::string>{"/h/dir/t.cpp", "-I", "/w/tree/include",
                                                "-o", "/b/dir/t"}));
}

TEST(EvalHiddenTest, ParsesTestCounts) {
  cf::eval::TestCounts counts;
  EXPECT_TRUE(cf::eval::parse_test_counts("noise\n{\"tests\": 4, \"passed\": 3}\n",
                                          &counts));
  EXPECT_EQ(counts.tests, 4);
  EXPECT_EQ(counts.passed, 3);

  EXPECT_FALSE(cf::eval::parse_test_counts("{\"foo\": 1}\n", &counts));
  EXPECT_FALSE(cf::eval::parse_test_counts("", &counts));
}

TEST(EvalHiddenTest, RunsCompiledHiddenTests) {
  const auto dir = make_temp_dir();
  write_file(dir / "hidden_test.cpp", R"cpp(
#include <cstdio>
int main() {
  std::printf("{\"tests\": 2, \"passed\": 2}\n");
  return 0;
}
)cpp");
  const auto spec_path = dir / "spec.yaml";
  write_file(spec_path, R"yaml(
compile:
  - name: build
    argv: [g++, -std=c++17, "{hidden}/hidden_test.cpp", -o, "{build}/hidden_test"]
tests:
  - name: run
    argv: ["{build}/hidden_test"]
)yaml");
  auto spec = cf::eval::load_hidden_test_spec(spec_path);
  ASSERT_TRUE(spec.has_value()) << spec.error().message;

  cf::process::LocalProcessExecutor executor;
  const auto result = cf::eval::run_hidden_tests(*spec, dir, dir / "build", executor,
                                                 std::chrono::seconds(120));
  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.tests, 2);
  EXPECT_EQ(result.passed, 2);

  // A test that exits non-zero without counts counts as one failing test.
  const auto failing_dir = make_temp_dir();
  write_file(failing_dir / "failing.cpp", "int main() { return 3; }\n");
  write_file(failing_dir / "spec.yaml", R"yaml(
compile:
  - argv: [g++, -std=c++17, "{hidden}/failing.cpp", -o, "{build}/failing"]
tests:
  - name: fails
    argv: ["{build}/failing"]
)yaml");
  auto failing_spec = cf::eval::load_hidden_test_spec(failing_dir / "spec.yaml");
  ASSERT_TRUE(failing_spec.has_value());
  const auto failing = cf::eval::run_hidden_tests(*failing_spec, failing_dir,
                                                  failing_dir / "build", executor,
                                                  std::chrono::seconds(120));
  EXPECT_FALSE(failing.ok);
  EXPECT_EQ(failing.tests, 1);
  EXPECT_EQ(failing.passed, 0);

  // A compile failure fails every test without running them.
  const auto broken_dir = make_temp_dir();
  write_file(broken_dir / "broken.cpp", "int main() { this is not C++ }\n");
  write_file(broken_dir / "spec.yaml", R"yaml(
compile:
  - argv: [g++, -std=c++17, "{hidden}/broken.cpp", -o, "{build}/broken"]
tests:
  - name: one
    argv: ["{build}/broken"]
  - name: two
    argv: ["{build}/broken"]
)yaml");
  auto broken_spec = cf::eval::load_hidden_test_spec(broken_dir / "spec.yaml");
  ASSERT_TRUE(broken_spec.has_value());
  const auto broken = cf::eval::run_hidden_tests(*broken_spec, broken_dir,
                                                 broken_dir / "build", executor,
                                                 std::chrono::seconds(120));
  EXPECT_FALSE(broken.ok);
  EXPECT_EQ(broken.tests, 2);
  EXPECT_EQ(broken.passed, 0);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove_all(failing_dir, ec);
  std::filesystem::remove_all(broken_dir, ec);
}

}  // namespace
