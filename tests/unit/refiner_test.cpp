// Unit tests for the plan refiner (design section 6.2).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cf/orch/refiner.hpp"

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
     "alternatives": ["hexagonal"]}
  ]
})json";

const char* kOkCritique = R"json({"verdict": "ok", "issues": []})json";

TEST(RefinerTest, RefinesAndKeepsThePlanContract) {
  ScriptedModel model;
  model.replies = {kDesign, kOkCritique};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner"});

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

TEST(RefinerTest, RevisionFollowsTheCritique) {
  ScriptedModel model;
  std::string revised(kDesign);
  const auto at = revised.find("\"title\": \"Core\"");
  revised.replace(at, std::string("\"title\": \"Core\"").size(),
                  "\"title\": \"Core revised\"");
  model.replies = {kDesign,
                   R"json({"verdict": "revise", "issues": ["M1 needs a mutex"]})json",
                   revised};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner"});

  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones[0].title, "Core revised");
  ASSERT_EQ(model.prompts.size(), 3u);
  EXPECT_NE(model.prompts[2].find("M1 needs a mutex"), std::string::npos);
}

TEST(RefinerTest, KeepsTheFirstDesignWhenTheCritiqueIsUnreadable) {
  ScriptedModel model;
  model.replies = {kDesign, "I could not review this."};
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner"});
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
  DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner"});
  auto package = refiner.refine(sample_plan());
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones[0].title, "Core");
}

TEST(RefinerTest, RejectsDesignsThatBreakThePlan) {
  const auto refine_with = [](const std::string& design) {
    ScriptedModel model;
    model.replies = {design, kOkCritique};
    DesignRefiner refiner(model, nullptr, DesignRefinerOptions{"test-planner"});
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

TEST(RefinerTest, ToleratesProseAroundTheJson) {
  const std::string text =
      std::string("Here is the design:\n```json\n") + kDesign + "\n```\nDone.";
  auto package = cf::orch::parse_design_package(text);
  ASSERT_TRUE(package.has_value()) << package.error().message;
  EXPECT_EQ(package->milestones.size(), 2u);
}

}  // namespace
