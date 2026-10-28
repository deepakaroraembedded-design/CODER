// cf-github: GitHub ruleset payloads (design section 10.4.7).
//
// The CF bot has no administration permission, so it never applies these
// rulesets itself. A human with admin rights generates the payloads and
// applies them with the GitHub API/CLI; this keeps the GitHub-side rules
// independent of the orchestrator.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cf::github {

struct RulesetOptions {
  std::string protected_name = "cf-protected-branches";
  std::string cf_name = "cf-bot-branches";
  // ~DEFAULT_BRANCH is GitHub's default-branch condition token.
  std::string default_branch = "~DEFAULT_BRANCH";
  std::vector<std::string> release_patterns = {"refs/heads/release/*"};
  std::string cf_pattern = "refs/heads/cf/**";
  std::vector<std::string> required_checks;  // status-check contexts
  int required_approvals = 1;
};

// Ruleset for main/release: PR review, CODEOWNERS, signatures, linear
// history, no force push or deletion.
nlohmann::json protected_branch_ruleset(const RulesetOptions& options);

// Ruleset for cf/**: signed commits, no force push. Deletion stays available
// to the bot for retention cleanup.
nlohmann::json cf_branch_ruleset(const RulesetOptions& options);

}  // namespace cf::github
