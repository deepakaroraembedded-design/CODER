// cf-eval: plan run outcomes (one record per plan per repeat) and their JSON
// representation, the input of the scoring stage (design section 17.2).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/eval/suite.hpp"

namespace cf::eval {

struct PlanOutcome {
  std::string plan_id;
  int repeat = 0;
  bool terminal = true;  // the run ended in a terminal state
  int milestones_total = 0;
  int milestones_done = 0;
  int hidden_tests_passed = 0;
  int hidden_tests_total = 0;
  int iterations = 0;
  double cost_usd = 0.0;
  int escalations = 0;
  std::vector<std::string> escalation_reasons;
  double duration_s = 0.0;
  bool security_passed = true;
  bool recovery_ok = true;
};

// All outcomes of one suite run under one configuration.
struct OutcomeSet {
  std::string suite_id;
  std::string config;  // e.g. "fireworks-default"
  std::vector<PlanOutcome> outcomes;
};

Result<OutcomeSet> parse_outcomes(std::string_view json);
Result<OutcomeSet> load_outcomes(const std::filesystem::path& path);
std::string outcomes_to_json(const OutcomeSet& set);

}  // namespace cf::eval
