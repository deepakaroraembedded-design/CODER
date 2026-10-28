#include "cf/github/rulesets.hpp"

namespace cf::github {

using Json = nlohmann::json;

Json protected_branch_ruleset(const RulesetOptions& options) {
  Json include = Json::array({options.default_branch});
  for (const auto& pattern : options.release_patterns) {
    include.push_back(pattern);
  }
  Json rules = Json::array();
  rules.push_back(Json{
      {"type", "pull_request"},
      {"parameters",
       Json{{"required_approving_review_count", options.required_approvals},
            {"dismiss_stale_reviews_on_push", true},
            {"require_code_owner_review", true},
            {"require_last_push_approval", true},
            {"required_review_thread_resolution", true}}}});
  if (!options.required_checks.empty()) {
    Json checks = Json::array();
    for (const auto& context : options.required_checks) {
      checks.push_back(Json{{"context", context}});
    }
    rules.push_back(Json{{"type", "required_status_checks"},
                         {"parameters",
                          Json{{"strict_required_status_checks_policy", true},
                               {"required_status_checks", std::move(checks)}}}});
  }
  rules.push_back(Json{{"type", "required_signatures"}});
  rules.push_back(Json{{"type", "required_linear_history"}});
  rules.push_back(Json{{"type", "non_fast_forward"}});
  rules.push_back(Json{{"type", "deletion"}});
  return Json{{"name", options.protected_name},
              {"target", "branch"},
              {"enforcement", "active"},
              {"conditions",
               Json{{"ref_name",
                     Json{{"include", std::move(include)}, {"exclude", Json::array()}}}}},
              {"rules", std::move(rules)},
              {"bypass_actors", Json::array()}};
}

Json cf_branch_ruleset(const RulesetOptions& options) {
  return Json{
      {"name", options.cf_name},
      {"target", "branch"},
      {"enforcement", "active"},
      {"conditions",
       Json{{"ref_name",
             Json{{"include", Json::array({options.cf_pattern})},
                  {"exclude", Json::array()}}}}},
      {"rules",
       Json::array({Json{{"type", "required_signatures"}},
                    Json{{"type", "non_fast_forward"}}})},
      {"bypass_actors", Json::array()}};
}

}  // namespace cf::github
