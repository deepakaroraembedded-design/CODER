// Unit tests for the plan refiner (design section 6.2).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "cf/orch/refiner.hpp"
#include "cf/rag/catalog.hpp"
#include "cf/rag/loader.hpp"

namespace {

using cf::orch::DesignModel;
using cf::orch::DesignPackage;
using cf::orch::DesignRefiner;
using cf::orch::DesignRefinerOptions;
using cf::orch::design_package_to_plan;

class ScriptedModel final : public DesignModel {
 public:
  std::vector<std::string> replies;
  std::vector<std::string> prompts;

  cf::Result<std::string> complete(std::string_view prompt) override {
    prompts.push_back(std::string(prompt));
    if (replies.empty()) {
      return cf::fail("TEST_NO_REPLY", "the script has no reply left");
    }
    std::string reply = replies.front();
    replies.erase(replies.begin());
    return reply;
  }
};

cf::plan::Plan sample_plan() {
  cf::plan::Plan plan;
  plan.plan_id = "PLAN-X";
  plan.product = "Y";
  plan.version = "1.0";
  plan.target.language = "cpp";
  plan.target.standard = "c++20";
  plan.target.build_system = "cmake";
  plan.target.test_framework = "gtest";
  cf::plan::MilestoneSpec core;
  core.id = "M1";
  core.title = "Core";
  core.requirements = {"REQ-1"};
  core.acceptance = {"core builds"};
  cf::plan::MilestoneSpec api;
  api.id = "M2";
  api.title = "API";
  api.requirements = {"REQ-2"};
  api.depends_on = {"M1"};
  api.acceptance = {"api builds"};
  plan.milestones = {core, api};
  return plan;
}

const char* kDesign = R"json({
  "system_design": "Layered: core below api.",
  "milestones": [
    {"id": "M1", "title": "Core", "requirements": ["REQ-1"], "depends_on": [],
     "acceptance": ["core builds"], "writes": ["src/core"],
     "uses": [], "writes_interfaces": ["include/core.hpp"]},
    {"id": "M2", "title": "API", "requirements": ["REQ-2"], "depends_on": ["M1"],
     "acceptance": ["api builds"], "writes": ["src/api"],
     "uses": ["include/core.hpp"], "writes_interfaces": []}
  ],
  "adrs": [
    {"id": "ADR-001", "title": "Layered design", "decision": "Use layers",
     "alternatives": ["hexagonal"], "pattern_id": "PAT-PRODUCER-CONSUMER"}
  ]
})json";

const char* kOkCritique = R"json({"verdict": "ok", "issues": []})json";

TEST(RefinerTest, RefinesAndKeepsThePlanContract) {
  ScriptedModel model;
  model.replies = {kDesign, kOkCritique};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner", 6, true, ""});

  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->plan_id, "PLAN-X");
  EXPECT_EQ(package->product, "Y");
  EXPECT_EQ(package->model, "test-planner");
  ASSERT_EQ(package->milestones.size(), 2u);
  EXPECT_EQ(package->milestones[0].writes_interfaces,
            (std::vector<std::string>{"include/core.hpp"}));
  ASSERT_EQ(package->adrs.size(), 1u);
  EXPECT_EQ(model.prompts.size(), 2u);  // design + self-critique
  EXPECT_NE(model.prompts[0].find("PLAN-X"), std::string::npos);
}

TEST(RefinerTest, HumanFeedbackIsCarriedIntoThePlannerPrompt) {
  ScriptedModel model;
  model.replies = {kDesign, kOkCritique};
  DesignRefinerOptions options{"test-planner", 6, true,
                               "Route construction through the registry factory."};
  DesignRefiner refiner(model, nullptr, std::move(options));
  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  ASSERT_FALSE(model.prompts.empty());
  EXPECT_NE(model.prompts[0].find("Route construction through the registry factory."),
            std::string::npos);
}

TEST(RefinerTest, PatternConformanceRequiresAKnownCatalogId) {
  auto entry = cf::rag::parse_pattern_entry(R"yaml(
id: PAT-PRODUCER-CONSUMER
name: Producer-consumer queue
level: system
use_when: ["producers hand work to consumers"]
)yaml",
                                            "pc.yaml");
  ASSERT_TRUE(entry.has_value()) << entry.error().message;
  const std::vector<cf::rag::PatternEntry> catalog = {*entry};

  auto package = cf::orch::parse_design_package(kDesign);
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_TRUE(
      cf::orch::validate_pattern_conformance(*package, catalog).has_value());

  auto missing = *package;
  missing.adrs[0].pattern_id.clear();
  auto missing_status = cf::orch::validate_pattern_conformance(missing, catalog);
  ASSERT_FALSE(missing_status.has_value());
  EXPECT_NE(missing_status.error().message.find("pattern_id"),
            std::string::npos);

  auto unknown = *package;
  unknown.adrs[0].pattern_id = "PAT-NOT-THERE";
  auto unknown_status = cf::orch::validate_pattern_conformance(unknown, catalog);
  ASSERT_FALSE(unknown_status.has_value());
  EXPECT_NE(unknown_status.error().message.find("PAT-NOT-THERE"),
            std::string::npos);
}

TEST(RefinerTest, ThePlannerPromptCarriesThePatternCatalog) {
  auto entry = cf::rag::parse_pattern_entry(R"yaml(
id: PAT-PRODUCER-CONSUMER
name: Producer-consumer queue
level: system
use_when: ["producers hand work to consumers"]
company_notes: "Fixed-capacity ring buffer with acquire/release atomics."
)yaml",
                                            "pc.yaml");
  ASSERT_TRUE(entry.has_value()) << entry.error().message;
  const std::vector<cf::rag::PatternEntry> catalog = {*entry};

  ScriptedModel model;
  model.replies = {kDesign, kOkCritique};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner", 6, true, ""},
                        &catalog);
  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  ASSERT_FALSE(model.prompts.empty());
  EXPECT_NE(model.prompts[0].find("Pattern catalog"), std::string::npos);
  EXPECT_NE(model.prompts[0].find("PAT-PRODUCER-CONSUMER"), std::string::npos);
  EXPECT_NE(model.prompts[0].find("acquire/release atomics"), std::string::npos);
}

TEST(RefinerTest, RevisionFollowsTheCritique) {
  ScriptedModel model;
  std::string revised(kDesign);
  const auto at = revised.find("\"title\": \"Core\"");
  revised.replace(at, std::string("\"title\": \"Core\"").size(),
                  "\"title\": \"Core revised\"");
  model.replies = {kDesign,
                   R"json({"verdict": "revise", "issues": ["M1 needs a mutex"]})json",
                   revised};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner", 6, true, ""});

  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones[0].title, "Core revised");
  ASSERT_EQ(model.prompts.size(), 3u);
  EXPECT_NE(model.prompts[2].find("M1 needs a mutex"), std::string::npos);
}

TEST(RefinerTest, KeepsTheFirstDesignWhenTheCritiqueIsUnreadable) {
  ScriptedModel model;
  model.replies = {kDesign, "I could not review this."};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner", 6, true, ""});
  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones[0].title, "Core");
}

TEST(RefinerTest, KeepsTheFirstDesignWhenTheRevisionIsInvalid) {
  ScriptedModel model;
  // The revision drops M2's dependency on M1: validation rejects it and the
  // first, valid design stands.
  model.replies = {kDesign,
                   R"json({"verdict": "revise", "issues": ["x"]})json",
                   R"json({"system_design": "x",
                           "milestones": [
                             {"id": "M1", "requirements": ["REQ-1"], "acceptance": ["a"]},
                             {"id": "M2", "requirements": ["REQ-2"], "acceptance": ["a"]}
                           ],
                           "adrs": [{"id": "ADR-001", "title": "t", "decision": "d"}]})json"};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner", 6, true, ""});
  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones[0].title, "Core");
}

TEST(RefinerTest, RejectsDesignsThatBreakThePlan) {
  const auto refine_with = [](const std::string& design) {
    ScriptedModel model;
    model.replies = {design, kOkCritique};
    DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner", 6, true, ""});
    return refiner.refine(sample_plan());
  };

  // Milestone ids must match the plan.
  auto wrong_ids = refine_with(R"json({"system_design": "x",
    "milestones": [{"id": "M9", "requirements": ["REQ-1"], "acceptance": ["a"]}],
    "adrs": [{"id": "ADR-1", "title": "t", "decision": "d"}]})json");
  EXPECT_FALSE(wrong_ids.has_value());
  EXPECT_EQ(wrong_ids.error().code, "ORCH_DESIGN_INVALID");

  // Requirement mappings are fixed.
  auto wrong_requirements = refine_with(R"json({"system_design": "x",
    "milestones": [
      {"id": "M1", "requirements": ["REQ-2"], "acceptance": ["a"]},
      {"id": "M2", "requirements": ["REQ-1"], "acceptance": ["a"], "depends_on": ["M1"]}
    ],
    "adrs": [{"id": "ADR-1", "title": "t", "decision": "d"}]})json");
  EXPECT_FALSE(wrong_requirements.has_value());

  // Plan dependencies cannot be dropped.
  auto dropped_dependency = refine_with(R"json({"system_design": "x",
    "milestones": [
      {"id": "M1", "requirements": ["REQ-1"], "acceptance": ["a"]},
      {"id": "M2", "requirements": ["REQ-2"], "acceptance": ["a"]}
    ],
    "adrs": [{"id": "ADR-1", "title": "t", "decision": "d"}]})json");
  EXPECT_FALSE(dropped_dependency.has_value());

  // ADRs are required.
  auto no_adrs = refine_with(R"json({"system_design": "x",
    "milestones": [
      {"id": "M1", "requirements": ["REQ-1"], "acceptance": ["a"]},
      {"id": "M2", "requirements": ["REQ-2"], "acceptance": ["a"], "depends_on": ["M1"]}
    ],
    "adrs": []})json");
  EXPECT_FALSE(no_adrs.has_value());

  // Not JSON at all.
  auto garbage = refine_with("no design here");
  EXPECT_FALSE(garbage.has_value());
  EXPECT_EQ(garbage.error().code, "ORCH_DESIGN_INVALID");
}

TEST(RefinerTest, DesignPackageToPlanCarriesTheWriteSets) {
  auto package = cf::orch::parse_design_package(kDesign);
  ASSERT_TRUE(package.has_value()) << package.error().message;
  const cf::plan::Plan refined =
      design_package_to_plan(*package, sample_plan());
  ASSERT_EQ(refined.milestones.size(), 2u);
  EXPECT_EQ(refined.milestones[0].writes, (std::vector<std::string>{"src/core"}));
  EXPECT_EQ(refined.milestones[0].writes_interfaces,
            (std::vector<std::string>{"include/core.hpp"}));
  EXPECT_EQ(refined.milestones[1].uses, (std::vector<std::string>{"include/core.hpp"}));
  // The plan contract is preserved.
  EXPECT_EQ(refined.plan_id, "PLAN-X");
  EXPECT_EQ(refined.target.language, "cpp");
  EXPECT_EQ(refined.milestones[1].depends_on, (std::vector<std::string>{"M1"}));
}

TEST(RefinerTest, PublishesApprovedDesignsAndSupersedesOlderOnes) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_refiner_publish_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  auto package = cf::orch::parse_design_package(kDesign);
  ASSERT_TRUE(package.has_value()) << package.error().message;
  // The refiner stamps these from the plan before the package is stored.
  package->plan_id = "PLAN-X";
  package->product = "Y";

  auto first = cf::orch::publish_design_package(dir, "R1", *package, 1000);
  ASSERT_TRUE(first.has_value()) << first.error().message;
  EXPECT_EQ(first->filename().string(), "design-PLAN-X-R1.md");

  // The document is a front-matter design document scoped to the product.
  const std::string text = [&] {
    std::ifstream in(*first, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }();
  EXPECT_NE(text.find("doc_id: design-PLAN-X-R1"), std::string::npos);
  EXPECT_NE(text.find("type: design"), std::string::npos);
  EXPECT_NE(text.find("scope_value: Y"), std::string::npos);
  EXPECT_NE(text.find("status: approved"), std::string::npos);
  EXPECT_NE(text.find("source: cf://runs/R1"), std::string::npos);
  EXPECT_NE(text.find("approved_at: 1000"), std::string::npos);
  EXPECT_NE(text.find("# Design package: PLAN-X"), std::string::npos);

  // A later approval for the same plan supersedes the earlier document.
  auto second = cf::orch::publish_design_package(dir, "R2", *package, 2000);
  ASSERT_TRUE(second.has_value()) << second.error().message;
  const std::string first_text = [&] {
    std::ifstream in(*first, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }();
  EXPECT_NE(first_text.find("status: superseded"), std::string::npos);
  const std::string second_text = [&] {
    std::ifstream in(*second, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }();
  EXPECT_NE(second_text.find("status: approved"), std::string::npos);

  // Both load back through the document loader (front matter is valid).
  auto loaded = cf::rag::load_document_dir(dir / "designs");
  ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
  EXPECT_EQ(loaded->size(), 2u);

  std::filesystem::remove_all(dir, ec);
}

TEST(RefinerTest, AbortingARunWithdrawsItsDesign) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_refiner_withdraw_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  auto package = cf::orch::parse_design_package(kDesign);
  ASSERT_TRUE(package.has_value());
  package->plan_id = "PLAN-X";
  package->product = "Y";
  auto other = *package;
  other.plan_id = "PLAN-Z";

  ASSERT_TRUE(cf::orch::publish_design_package(dir, "R1", *package, 1000).has_value());
  ASSERT_TRUE(cf::orch::publish_design_package(dir, "R2", other, 2000).has_value());

  // Aborting R1 withdraws only R1's document.
  auto withdrawn = cf::orch::withdraw_design_package(dir, "R1");
  ASSERT_TRUE(withdrawn.has_value()) << withdrawn.error().message;
  EXPECT_EQ(*withdrawn, 1u);
  const auto load = [&dir](const std::string& run_id) {
    std::ifstream in(dir / "designs" / ("design-" + std::string("PLAN-") +
                                        (run_id == "R1" ? "X" : "Z") + "-" + run_id +
                                        ".md"),
                     std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  };
  EXPECT_NE(load("R1").find("status: withdrawn"), std::string::npos);
  EXPECT_NE(load("R2").find("status: approved"), std::string::npos);

  // Withdrawing again is a no-op; an unknown run withdraws nothing.
  EXPECT_EQ(*cf::orch::withdraw_design_package(dir, "R1"), 0u);
  EXPECT_EQ(*cf::orch::withdraw_design_package(dir, "R9"), 0u);
  std::filesystem::remove_all(dir, ec);
}

TEST(RefinerTest, ToleratesProseAroundTheJson) {
  const std::string text =
      std::string("Here is the design:\n```json\n") + kDesign + "\n```\nDone.";
  auto package = cf::orch::parse_design_package(text);
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones.size(), 2u);
}

}  // namespace
