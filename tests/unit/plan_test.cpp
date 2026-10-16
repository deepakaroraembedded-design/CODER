// Unit tests for the plan parser and validator (design sections 6.2, 8.1).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/plan/plan.hpp"

namespace {

using cf::plan::MilestoneSpec;
using cf::plan::Plan;
using cf::plan::TargetSpec;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_plan_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

std::string minimal_plan(const std::string& milestone_block,
                         const std::string& extra = "") {
  return std::string("plan_id: P1\n") + "product: X\n" + "version: 1\n" +
         "milestones:\n" + milestone_block +
         "target:\n"
         "  language: cpp\n"
         "  standard: c++20\n"
         "  build_system: cmake\n"
         "  test_framework: gtest\n" +
         extra;
}

const char* kExamplePlan = R"yaml(
plan_id: PLAN-MULTICODER-X
product: Y
version: 1.0
requirements_ref: repo://docs/requirements/Y-SRS.md@v3.2
milestones:
  - id: M1
    title: Core message bus
    requirements: [REQ-001, REQ-002]
    depends_on: []
    acceptance:
      - "Publish/subscribe latency p99 < 2 ms in-process"
      - "No data races under TSan with 16 producers"
  - id: M2
    title: Agent registry
    requirements: [REQ-010]
    depends_on: [M1]
target:
  language: cpp
  standard: c++20
  build_system: cmake
  test_framework: gtest
  platforms: [linux-x86_64]
constraints:
  forbidden: ["exceptions across module boundaries"]
models:
  coder:    deepseek
  reviewer: kimi
  qa:       glm
inference: fireworks
budget:
  max_usd_total: 250
  max_wall_clock_hours: 36
)yaml";

TEST(PlanTest, ParsesTheExamplePlan) {
  auto plan = cf::plan::parse_plan_string(kExamplePlan);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->plan_id, "PLAN-MULTICODER-X");
  EXPECT_EQ(plan->product, "Y");
  EXPECT_EQ(plan->version, "1.0");
  EXPECT_EQ(plan->requirements_ref, "repo://docs/requirements/Y-SRS.md@v3.2");
  ASSERT_EQ(plan->milestones.size(), 2u);
  EXPECT_EQ(plan->milestones[0].id, "M1");
  EXPECT_EQ(plan->milestones[0].title, "Core message bus");
  ASSERT_EQ(plan->milestones[0].requirements.size(), 2u);
  EXPECT_EQ(plan->milestones[0].requirements[0], "REQ-001");
  EXPECT_TRUE(plan->milestones[0].depends_on.empty());
  ASSERT_EQ(plan->milestones[0].acceptance.size(), 2u);
  EXPECT_EQ(plan->milestones[1].depends_on, std::vector<std::string>{"M1"});
  EXPECT_EQ(plan->target.language, "cpp");
  EXPECT_EQ(plan->target.standard, "c++20");
  EXPECT_EQ(plan->target.build_system, "cmake");
  EXPECT_EQ(plan->target.test_framework, "gtest");
  EXPECT_EQ(plan->target.platforms, std::vector<std::string>{"linux-x86_64"});
  EXPECT_EQ(plan->forbidden, std::vector<std::string>{"exceptions across module boundaries"});
  ASSERT_EQ(plan->role_models.size(), 3u);
  EXPECT_EQ(plan->role_models.at("coder"), "deepseek");
  EXPECT_EQ(plan->role_models.at("reviewer"), "kimi");
  EXPECT_EQ(plan->role_models.at("qa"), "glm");
  EXPECT_EQ(plan->inference, "fireworks");
  ASSERT_TRUE(plan->budget.max_usd_total.has_value());
  EXPECT_DOUBLE_EQ(*plan->budget.max_usd_total, 250.0);
  ASSERT_TRUE(plan->budget.max_wall_clock_hours.has_value());
  EXPECT_DOUBLE_EQ(*plan->budget.max_wall_clock_hours, 36.0);
}

TEST(PlanTest, ParsesMarkdownWithFrontMatter) {
  const std::string text =
      "---\n" + minimal_plan("  - id: M1\n    title: T\n") + "---\n\n# Plan\n\nBody text.\n";
  auto plan = cf::plan::parse_plan_string(text);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->plan_id, "P1");
}

TEST(PlanTest, UnterminatedFrontMatterFails) {
  const std::string text = "---\n" + minimal_plan("  - id: M1\n    title: T\n");
  auto plan = cf::plan::parse_plan_string(text);
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kParseFailed);
}

TEST(PlanTest, MissingPlanIdFails) {
  auto plan = cf::plan::parse_plan_string(
      "product: X\nversion: 1\nmilestones:\n  - id: M1\n    title: T\ntarget:\n"
      "  language: cpp\n  standard: c++20\n  build_system: cmake\n"
      "  test_framework: gtest\n");
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kSchemaInvalid);
}

TEST(PlanTest, MissingTargetFails) {
  auto plan = cf::plan::parse_plan_string(
      "plan_id: P1\nproduct: X\nversion: 1\nmilestones:\n  - id: M1\n    title: T\n");
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kSchemaInvalid);
}

TEST(PlanTest, MissingMilestonesFails) {
  auto plan = cf::plan::parse_plan_string(
      "plan_id: P1\nproduct: X\nversion: 1\ntarget:\n  language: cpp\n"
      "  standard: c++20\n  build_system: cmake\n  test_framework: gtest\n");
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kSchemaInvalid);
}

TEST(PlanTest, EmptyMilestoneListFails) {
  auto plan = cf::plan::parse_plan_string(minimal_plan("  []\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kSchemaInvalid);
}

TEST(PlanTest, DuplicateMilestoneIdFails) {
  auto plan = cf::plan::parse_plan_string(
      minimal_plan("  - id: M1\n    title: A\n  - id: M1\n    title: B\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kDependencyInvalid);
}

TEST(PlanTest, UnknownDependencyFails) {
  auto plan = cf::plan::parse_plan_string(
      minimal_plan("  - id: M1\n    title: A\n    depends_on: [M9]\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kDependencyInvalid);
}

TEST(PlanTest, DependencyCycleFails) {
  auto plan = cf::plan::parse_plan_string(minimal_plan(
      "  - id: M1\n    title: A\n    depends_on: [M2]\n"
      "  - id: M2\n    title: B\n    depends_on: [M1]\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kDependencyInvalid);
}

TEST(PlanTest, SelfDependencyFails) {
  auto plan = cf::plan::parse_plan_string(
      minimal_plan("  - id: M1\n    title: A\n    depends_on: [M1]\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kDependencyInvalid);
}

TEST(PlanTest, InvalidInferenceFails) {
  auto plan = cf::plan::parse_plan_string(
      minimal_plan("  - id: M1\n    title: A\n", "inference: some-cloud\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kSchemaInvalid);
}

TEST(PlanTest, LocalVllmInferenceAccepted) {
  auto plan = cf::plan::parse_plan_string(
      minimal_plan("  - id: M1\n    title: A\n", "inference: local-vllm\n"));
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->inference, "local-vllm");
}

TEST(PlanTest, UnknownModelRoleFails) {
  auto plan = cf::plan::parse_plan_string(
      minimal_plan("  - id: M1\n    title: A\n", "models:\n  tester: glm\n"));
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kSchemaInvalid);
}

TEST(PlanTest, ProfileLookupRejectsUnknownTarget) {
  cf::plan::ValidationOptions options;
  options.profile_exists = [](const TargetSpec&) { return false; };
  auto plan = cf::plan::parse_plan_string(minimal_plan("  - id: M1\n    title: A\n"),
                                          options);
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kNoToolchainProfile);
}

TEST(PlanTest, ProfileLookupAcceptsKnownTarget) {
  cf::plan::ValidationOptions options;
  options.profile_exists = [](const TargetSpec& target) {
    return target.language == "cpp" && target.test_framework == "gtest";
  };
  auto plan = cf::plan::parse_plan_string(minimal_plan("  - id: M1\n    title: A\n"),
                                          options);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
}

TEST(PlanTest, TopologicalOrderRespectsDependencies) {
  auto plan = cf::plan::parse_plan_string(minimal_plan(
      "  - id: M3\n    title: C\n    depends_on: [M1, M2]\n"
      "  - id: M1\n    title: A\n"
      "  - id: M2\n    title: B\n    depends_on: [M1]\n"));
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  const std::vector<std::string> order = plan->topological_order();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], "M1");
  EXPECT_EQ(order[1], "M2");
  EXPECT_EQ(order[2], "M3");
}

TEST(PlanTest, ParsesWriteAndUseSets) {
  auto plan = cf::plan::parse_plan_string(minimal_plan(
      "  - id: M1\n    title: A\n    writes: [src/a.cpp, src/a.hpp]\n"
      "    uses: [IFoo]\n    writes_interfaces: [IBar]\n"));
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->milestones[0].writes,
            (std::vector<std::string>{"src/a.cpp", "src/a.hpp"}));
  EXPECT_EQ(plan->milestones[0].uses, (std::vector<std::string>{"IFoo"}));
  EXPECT_EQ(plan->milestones[0].writes_interfaces,
            (std::vector<std::string>{"IBar"}));
}

TEST(PlanTest, FindMilestone) {
  auto plan = cf::plan::parse_plan_string(kExamplePlan);
  ASSERT_TRUE(plan.has_value());
  const MilestoneSpec* milestone = plan->find_milestone("M2");
  ASSERT_NE(milestone, nullptr);
  EXPECT_EQ(milestone->title, "Agent registry");
  EXPECT_EQ(plan->find_milestone("M9"), nullptr);
}

TEST(PlanTest, ParsesPlanFile) {
  const auto dir = make_temp_dir();
  const auto path = dir / "plan.md";
  {
    std::ofstream out(path);
    out << "---\n" << minimal_plan("  - id: M1\n    title: T\n") << "---\nBody.\n";
  }
  auto plan = cf::plan::parse_plan_file(path);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->plan_id, "P1");
  EXPECT_EQ(plan->source_path, path.string());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(PlanTest, MissingFileFails) {
  auto plan = cf::plan::parse_plan_file("/nonexistent/plan.yaml");
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kParseFailed);
}

TEST(PlanTest, MalformedYamlFails) {
  auto plan = cf::plan::parse_plan_string("plan_id: [unclosed\n");
  ASSERT_FALSE(plan.has_value());
  EXPECT_EQ(plan.error().code, cf::plan::error_codes::kParseFailed);
}

}  // namespace
