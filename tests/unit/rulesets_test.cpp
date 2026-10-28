// Unit tests for GitHub ruleset payload generation (design section 10.4.7).
#include <gtest/gtest.h>

#include <string>

#include "cf/github/rulesets.hpp"

namespace {

TEST(RulesetsTest, ProtectedBranchesRequireReviewSignaturesAndChecks) {
  cf::github::RulesetOptions options;
  options.required_checks = {"ci / build", "ci / test"};
  const auto ruleset = cf::github::protected_branch_ruleset(options);
  EXPECT_EQ(ruleset["name"], "cf-protected-branches");
  EXPECT_EQ(ruleset["target"], "branch");
  EXPECT_EQ(ruleset["enforcement"], "active");
  EXPECT_TRUE(ruleset["bypass_actors"].empty());
  EXPECT_EQ(ruleset["conditions"]["ref_name"]["include"][0], "~DEFAULT_BRANCH");
  EXPECT_EQ(ruleset["conditions"]["ref_name"]["include"][1],
            "refs/heads/release/*");

  bool review = false;
  bool signatures = false;
  bool linear = false;
  bool no_force = false;
  bool no_delete = false;
  bool checks = false;
  for (const auto& rule : ruleset["rules"]) {
    const std::string type = rule["type"];
    review = review || type == "pull_request";
    signatures = signatures || type == "required_signatures";
    linear = linear || type == "required_linear_history";
    no_force = no_force || type == "non_fast_forward";
    no_delete = no_delete || type == "deletion";
    if (type == "required_status_checks") {
      checks = rule["parameters"]["required_status_checks"].size() == 2 &&
               rule["parameters"]["strict_required_status_checks_policy"];
    }
  }
  EXPECT_TRUE(review);
  EXPECT_TRUE(signatures);
  EXPECT_TRUE(linear);
  EXPECT_TRUE(no_force);
  EXPECT_TRUE(no_delete);
  EXPECT_TRUE(checks);
}

TEST(RulesetsTest, CfBranchesRequireSignaturesAndBlockForcePush) {
  const auto ruleset = cf::github::cf_branch_ruleset(cf::github::RulesetOptions{});
  EXPECT_EQ(ruleset["name"], "cf-bot-branches");
  EXPECT_EQ(ruleset["conditions"]["ref_name"]["include"][0], "refs/heads/cf/**");
  bool signatures = false;
  bool no_force = false;
  for (const auto& rule : ruleset["rules"]) {
    const std::string type = rule["type"];
    signatures = signatures || type == "required_signatures";
    no_force = no_force || type == "non_fast_forward";
    EXPECT_NE(type, "deletion");  // retention cleanup still needs deletion
  }
  EXPECT_TRUE(signatures);
  EXPECT_TRUE(no_force);
}

}  // namespace
