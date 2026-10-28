// Unit tests for the staged gate runner (design section 6.7).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/gates/process.hpp"
#include "cf/gates/profile.hpp"
#include "cf/gates/runner.hpp"

namespace {

using cf::gates::GateDefinition;
using cf::gates::GateFinding;
using cf::gates::GateReport;
using cf::gates::GateRunOptions;
using cf::gates::GateRunner;
using cf::gates::GateStage;
using cf::gates::GateStep;
using cf::gates::LocalProcessExecutor;
using cf::gates::ProcessExecutor;
using cf::gates::ProcessRequest;
using cf::gates::ProcessResult;
using Json = nlohmann::json;

struct ScriptedProcess {
  bool launched = true;
  int exit_code = 0;
  bool timed_out = false;
  std::string error;
  std::string stdout_text;
  std::string stderr_text;
};

// Scripts process results by program name, so parallel stages stay
// deterministic. Programs without a script succeed.
class FakeExecutor final : public ProcessExecutor {
 public:
  ProcessResult run(const ProcessRequest& request) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(request);
      ++in_flight_;
      max_in_flight_ = std::max(max_in_flight_, in_flight_);
    }
    if (block_.count() > 0) {
      std::this_thread::sleep_for(block_);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }

    ScriptedProcess scripted;
    if (!request.argv.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = by_program_.find(request.argv[0]);
      if (it != by_program_.end()) {
        scripted = it->second;
      }
    }
    ProcessResult result;
    result.launched = scripted.launched;
    result.exit_code = scripted.exit_code;
    result.timed_out = scripted.timed_out;
    result.error = scripted.error;
    result.stdout_text = scripted.stdout_text;
    result.stderr_text = scripted.stderr_text;
    return result;
  }

  void script(const std::string& program, ScriptedProcess result) {
    std::lock_guard<std::mutex> lock(mutex_);
    by_program_[program] = std::move(result);
  }

  std::vector<ProcessRequest> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

  int max_in_flight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_in_flight_;
  }

  void set_block(std::chrono::milliseconds block) { block_ = block; }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ScriptedProcess> by_program_;
  std::vector<ProcessRequest> requests_;
  int in_flight_ = 0;
  int max_in_flight_ = 0;
  std::chrono::milliseconds block_{0};
};

GateStep step(const std::string& program) {
  GateStep s;
  s.argv = {program};
  return s;
}

GateDefinition gate(const std::string& name, GateStage stage,
                    const std::string& program) {
  GateDefinition definition;
  definition.name = name;
  definition.stage = stage;
  definition.steps = {step(program)};
  return definition;
}

std::vector<GateDefinition> standard_plan() {
  return {
      gate("build", GateStage::Prerequisites, "build-tool"),
      gate("format", GateStage::Cheap, "format-tool"),
      gate("guardrails", GateStage::Cheap, "guardrail-tool"),
      gate("coverage", GateStage::Expensive, "coverage-tool"),
  };
}

const GateFinding* find_finding(const GateReport& report, const std::string& gate_name) {
  for (const auto& result : report.results) {
    if (result.gate == gate_name && !result.findings.empty()) {
      return &result.findings.front();
    }
  }
  return nullptr;
}

TEST(GateRunnerTest, AllPassRunsEveryStage) {
  FakeExecutor executor;
  GateRunner runner(executor);
  const GateReport report = runner.run(standard_plan(), GateRunOptions{});
  EXPECT_TRUE(report.passed);
  ASSERT_EQ(report.results.size(), 4u);
  for (const auto& result : report.results) {
    EXPECT_TRUE(result.passed) << result.gate;
    EXPECT_FALSE(result.skipped) << result.gate;
  }
  EXPECT_EQ(executor.requests().size(), 4u);
}

TEST(GateRunnerTest, PrerequisiteFailureSkipsLaterStages) {
  FakeExecutor executor;
  executor.script("build-tool",
                  ScriptedProcess{true, 1, false, "", "", "build failed badly"});
  GateRunner runner(executor);
  const GateReport report = runner.run(standard_plan(), GateRunOptions{});

  EXPECT_FALSE(report.passed);
  ASSERT_EQ(report.results.size(), 4u);
  EXPECT_EQ(report.results[0].gate, "build");
  EXPECT_FALSE(report.results[0].passed);
  for (std::size_t i = 1; i < report.results.size(); ++i) {
    EXPECT_TRUE(report.results[i].skipped) << report.results[i].gate;
  }
  EXPECT_EQ(executor.requests().size(), 1u);
  const GateFinding* finding = find_finding(report, "build");
  ASSERT_NE(finding, nullptr);
  EXPECT_EQ(finding->severity, "error");
  EXPECT_NE(finding->message.find("build failed badly"), std::string::npos);
}

TEST(GateRunnerTest, CheapStageFailureSkipsExpensiveStage) {
  FakeExecutor executor;
  executor.script("format-tool", ScriptedProcess{true, 1, false, "", "", "bad format"});
  GateRunner runner(executor);
  const GateReport report = runner.run(standard_plan(), GateRunOptions{});

  EXPECT_FALSE(report.passed);
  EXPECT_FALSE(report.results[1].passed);  // format
  EXPECT_TRUE(report.results[2].passed);   // guardrails ran and passed
  EXPECT_TRUE(report.results[3].skipped);  // coverage skipped
  EXPECT_EQ(executor.requests().size(), 3u);
}

TEST(GateRunnerTest, MissingProgramIsReported) {
  FakeExecutor executor;
  executor.script("build-tool", ScriptedProcess{false, -1, false,
                                                "exec: No such file or directory", "", ""});
  GateRunner runner(executor);
  const GateReport report = runner.run(standard_plan(), GateRunOptions{});
  EXPECT_FALSE(report.passed);
  const GateFinding* finding = find_finding(report, "build");
  ASSERT_NE(finding, nullptr);
  EXPECT_NE(finding->message.find("failed to launch"), std::string::npos);
}

TEST(GateRunnerTest, TimeoutIsReported) {
  FakeExecutor executor;
  executor.script("build-tool", ScriptedProcess{true, -1, true, "", "", ""});
  GateRunner runner(executor);
  GateRunOptions options;
  options.step_timeout = std::chrono::milliseconds(1234);
  const GateReport report = runner.run(standard_plan(), options);
  EXPECT_FALSE(report.passed);
  const GateFinding* finding = find_finding(report, "build");
  ASSERT_NE(finding, nullptr);
  EXPECT_NE(finding->message.find("timed out after 1234 ms"), std::string::npos);
}

TEST(GateRunnerTest, UnknownPlaceholderFailsTheGateWithoutExecution) {
  FakeExecutor executor;
  GateDefinition bad;
  bad.name = "format";
  bad.stage = GateStage::Cheap;
  bad.steps.push_back(GateStep{{"tool", "{not_a_placeholder}"}});
  GateRunner runner(executor);
  const GateReport report = runner.run({bad}, GateRunOptions{});
  EXPECT_FALSE(report.passed);
  EXPECT_TRUE(executor.requests().empty());
  const GateFinding* finding = find_finding(report, "format");
  ASSERT_NE(finding, nullptr);
  EXPECT_NE(finding->message.find("unknown placeholder"), std::string::npos);
}

TEST(GateRunnerTest, IdenticalFindingsAcrossGatesAreDeduplicated) {
  FakeExecutor executor;
  executor.script("format-tool", ScriptedProcess{true, 1, false, "", "", "same error"});
  executor.script("guardrail-tool",
                  ScriptedProcess{true, 1, false, "", "", "same error"});
  GateRunner runner(executor);
  const GateReport report = runner.run(standard_plan(), GateRunOptions{});
  EXPECT_EQ(report.duplicate_findings_removed, 1u);
  std::size_t findings = 0;
  for (const auto& result : report.results) {
    findings += result.findings.size();
  }
  EXPECT_EQ(findings, 1u);
}

TEST(GateRunnerTest, FindingsAreCappedForThePrompt) {
  std::vector<GateDefinition> plan;
  for (int i = 0; i < 6; ++i) {
    const std::string program = "tool-" + std::to_string(i);
    plan.push_back(gate("gate-" + std::to_string(i), GateStage::Cheap, program));
  }
  FakeExecutor executor;
  for (int i = 0; i < 6; ++i) {
    executor.script("tool-" + std::to_string(i),
                    ScriptedProcess{true, 1, false, "", "",
                                    "error number " + std::to_string(i)});
  }
  GateRunner runner(executor);
  GateRunOptions options;
  options.max_findings = 3;
  const GateReport report = runner.run(plan, options);
  EXPECT_EQ(report.findings_truncated, 3u);
  std::size_t findings = 0;
  for (const auto& result : report.results) {
    findings += result.findings.size();
  }
  EXPECT_EQ(findings, 3u);
}

TEST(GateRunnerTest, CheapGatesRunInParallel) {
  std::vector<GateDefinition> plan;
  for (int i = 0; i < 3; ++i) {
    plan.push_back(gate("gate-" + std::to_string(i), GateStage::Cheap,
                        "tool-" + std::to_string(i)));
  }
  FakeExecutor executor;
  executor.set_block(std::chrono::milliseconds(40));
  GateRunner runner(executor);
  const GateReport report = runner.run(plan, GateRunOptions{});
  EXPECT_TRUE(report.passed);
  EXPECT_GE(executor.max_in_flight(), 2);
  // Results keep the plan order regardless of completion order.
  ASSERT_EQ(report.results.size(), 3u);
  EXPECT_EQ(report.results[0].gate, "gate-0");
  EXPECT_EQ(report.results[1].gate, "gate-1");
  EXPECT_EQ(report.results[2].gate, "gate-2");
}

TEST(GateRunnerTest, PlaceholdersAreExpandedBeforeExecution) {
  FakeExecutor executor;
  GateDefinition build = gate("build", GateStage::Prerequisites, "cmake");
  build.steps[0].argv = {"cmake", "-B", "{build_dir}"};
  GateRunner runner(executor);
  GateRunOptions options;
  options.step_context.build_dir = "/build";
  const GateReport report = runner.run({build}, options);
  EXPECT_TRUE(report.passed);
  const auto requests = executor.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].argv, (std::vector<std::string>{"cmake", "-B", "/build"}));
}

TEST(GateRunnerTest, ReportJsonCarriesTheAggregatedResult) {
  FakeExecutor executor;
  executor.script("build-tool", ScriptedProcess{true, 1, false, "", "", "boom"});
  GateRunner runner(executor);
  GateRunOptions options;
  options.commit_sha = "a1b2c3d";
  const GateReport report = runner.run(standard_plan(), options);

  const Json document = Json::parse(report.to_json());
  EXPECT_EQ(document["commit"], "a1b2c3d");
  EXPECT_FALSE(document["passed"].get<bool>());
  ASSERT_EQ(document["results"].size(), 4u);
  EXPECT_EQ(document["results"][0]["gate"], "build");
  EXPECT_FALSE(document["results"][0]["passed"].get<bool>());
  EXPECT_TRUE(document["results"][1]["skipped"].get<bool>());
  ASSERT_FALSE(document["results"][0]["findings"].empty());
  EXPECT_EQ(document["results"][0]["findings"][0]["severity"], "error");
}

TEST(GateRunnerTest, EndToEndWithLocalExecutor) {
  LocalProcessExecutor executor;
  GateRunner runner(executor);
  std::vector<GateDefinition> plan;
  plan.push_back(gate("build", GateStage::Prerequisites, "true"));
  plan.push_back(gate("format", GateStage::Cheap, "echo"));
  plan.push_back(gate("tests", GateStage::Expensive, "false"));
  const GateReport report = runner.run(plan, GateRunOptions{});
  EXPECT_FALSE(report.passed);
  ASSERT_EQ(report.results.size(), 3u);
  EXPECT_TRUE(report.results[0].passed);
  EXPECT_TRUE(report.results[1].passed);
  EXPECT_FALSE(report.results[2].passed);
  EXPECT_FALSE(report.results[2].skipped);
}

const char* kProfileYaml = R"yaml(
id: c-cpp
image: img
languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails:
      steps:
        - [clang-tidy, -p, "{build_dir}"]
    format:
      steps:
        - [clang-format, --dry-run, "{changed_files...}"]
build_systems:
  cmake:
    steps:
      - [cmake, -S, "{src_dir}", -B, "{build_dir}"]
test_frameworks:
  gtest: { steps: [[ctest, --test-dir, "{build_dir}"]], report: junit, languages: [cpp] }
common:
  analysis:
    steps:
      - [cppcheck, "{build_dir}"]
)yaml";

TEST(GatePlanTest, BuildsTheStandardStagedPlan) {
  auto profile = cf::gates::load_profile_string(kProfileYaml);
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  auto plan = cf::gates::build_gate_plan(*profile, "cpp", "cmake", "gtest");
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  ASSERT_EQ(plan->size(), 5u);
  EXPECT_EQ((*plan)[0].name, "build");
  EXPECT_EQ((*plan)[0].stage, GateStage::Prerequisites);
  EXPECT_EQ((*plan)[1].name, "format");
  EXPECT_EQ((*plan)[2].name, "guardrails");
  EXPECT_EQ((*plan)[3].name, "analysis");
  EXPECT_EQ((*plan)[4].name, "tests");
  for (std::size_t i = 1; i < plan->size(); ++i) {
    EXPECT_EQ((*plan)[i].stage, GateStage::Cheap);
  }
  ASSERT_EQ((*plan)[0].steps.size(), 1u);
  EXPECT_EQ((*plan)[0].steps[0].argv.front(), "cmake");
}

TEST(GatePlanTest, IncludesDeclaredCoverageAndMutationGates) {
  const char* profile_yaml = R"yaml(
id: stage3
image: img
languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails: { steps: [[true]] }
    format: { steps: [[true]] }
build_systems:
  cmake: { steps: [[true]] }
test_frameworks:
  gtest: { steps: [[true]], languages: [cpp] }
common:
  coverage:
    tool: cov
    line: 0.85
    branch: 0.75
    steps: [[cov, "{coverage_line}", "{coverage_branch}"]]
  mutation:
    tool: mut
    min_score: 0.60
    steps: [[mut, "{mutation_min_score}"]]
)yaml";
  auto profile = cf::gates::load_profile_string(profile_yaml);
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  auto plan = cf::gates::build_gate_plan(*profile, "cpp", "cmake", "gtest");
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  ASSERT_EQ(plan->size(), 6u);
  EXPECT_EQ((*plan)[4].name, "coverage");
  EXPECT_EQ((*plan)[4].stage, GateStage::Expensive);
  EXPECT_EQ((*plan)[5].name, "mutation");
  EXPECT_EQ((*plan)[5].stage, GateStage::Expensive);
}

TEST(CoverageReportTest, GateEnforcesThresholdsFromTheReport) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_coverage_gate_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir / "build");
  cf::process::LocalProcessExecutor executor;

  cf::gates::GateRunOptions options;
  options.working_directory = dir.string();
  options.step_context.build_dir = (dir / "build").string();

  cf::gates::GateDefinition gate;
  gate.name = "coverage";
  gate.stage = cf::gates::GateStage::Expensive;
  gate.steps.push_back(cf::gates::GateStep{
      {"sh", "-c", "printf 'a.go:1,2 2 1\\na.go:3,4 2 0\\n' > {build_dir}/coverage.out"}});
  gate.coverage = cf::gates::CoverageCheck{
      "go-cover", "{build_dir}/coverage.out", 0.5, 0.0};
  cf::gates::GateRunner runner(executor);
  auto report = runner.run({gate}, options);
  ASSERT_EQ(report.results.size(), 1u);
  EXPECT_TRUE(report.results[0].passed);
  EXPECT_TRUE(report.passed);

  gate.coverage.line = 0.9;
  report = runner.run({gate}, options);
  ASSERT_EQ(report.results.size(), 1u);
  EXPECT_FALSE(report.results[0].passed);
  ASSERT_FALSE(report.results[0].findings.empty());
  EXPECT_NE(report.results[0].findings[0].message.find("line coverage"),
            std::string::npos);
  std::filesystem::remove_all(dir, ec);
}

TEST(CoverageReportTest, ParsesGoCoverageJSONAndLcov) {
  auto go = cf::gates::parse_coverage_report(
      "go-cover", "file.go:1,2 3 1\nfile.go:3,4 2 0\n");
  ASSERT_TRUE(go.has_value()) << go.error().message;
  EXPECT_DOUBLE_EQ(go->line, 0.6);
  EXPECT_FALSE(go->has_branch);

  auto json = cf::gates::parse_coverage_report(
      "coverage-json",
      R"({"totals":{"covered_lines":8,"num_statements":10,"covered_branches":3,"num_branches":4}})");
  ASSERT_TRUE(json.has_value()) << json.error().message;
  EXPECT_DOUBLE_EQ(json->line, 0.8);
  ASSERT_TRUE(json->has_branch);
  EXPECT_DOUBLE_EQ(json->branch, 0.75);

  auto lcov = cf::gates::parse_coverage_report(
      "lcov",
      "TN:\nSF:a.cpp\nLF:10\nLH:8\nBRF:4\nBRH:3\nend_of_record\n");
  ASSERT_TRUE(lcov.has_value()) << lcov.error().message;
  EXPECT_DOUBLE_EQ(lcov->line, 0.8);
  ASSERT_TRUE(lcov->has_branch);
  EXPECT_DOUBLE_EQ(lcov->branch, 0.75);

  auto llvm = cf::gates::parse_coverage_report(
      "llvm-cov-json",
      R"({"data":[{"totals":{"lines":{"count":10,"covered":8,"percent":80},"branches":{"count":4,"covered":3,"percent":75}}}]})");
  ASSERT_TRUE(llvm.has_value()) << llvm.error().message;
  EXPECT_DOUBLE_EQ(llvm->line, 0.8);
  ASSERT_TRUE(llvm->has_branch);
  EXPECT_DOUBLE_EQ(llvm->branch, 0.75);

  auto jacoco = cf::gates::parse_coverage_report(
      "jacoco-xml",
      R"(<report><counter type="LINE" missed="2" covered="8"/><counter type="BRANCH" missed="1" covered="3"/></report>)");
  ASSERT_TRUE(jacoco.has_value()) << jacoco.error().message;
  EXPECT_DOUBLE_EQ(jacoco->line, 0.8);
  ASSERT_TRUE(jacoco->has_branch);
  EXPECT_DOUBLE_EQ(jacoco->branch, 0.75);

  auto unknown = cf::gates::parse_coverage_report("sarif", "{}");
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, cf::gates::error_codes::kCoverageReport);
}

TEST(GatePlanTest, RejectsUnknownCombinations) {
  auto profile = cf::gates::load_profile_string(kProfileYaml);
  ASSERT_TRUE(profile.has_value());
  EXPECT_FALSE(cf::gates::build_gate_plan(*profile, "rust", "cmake", "gtest").has_value());
  EXPECT_FALSE(cf::gates::build_gate_plan(*profile, "cpp", "bazel", "gtest").has_value());
  EXPECT_FALSE(cf::gates::build_gate_plan(*profile, "cpp", "cmake", "vitest").has_value());
}

}  // namespace
