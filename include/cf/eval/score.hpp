// cf-eval: scoring (design section 17.2).
//
// Per-plan scores, strata (category, language, tier, overall, held-out vs
// tuning) and target checks. Rate intervals are the pooled per-repeat samples
// of the stratum; the interval is the normal approximation mean ± 1.96·SE.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/eval/outcome.hpp"
#include "cf/eval/suite.hpp"

namespace cf::eval {

struct Interval {
  double mean = 0.0;
  double low = 0.0;
  double high = 0.0;
  std::size_t samples = 0;
};

struct PlanScore {
  std::string plan_id;
  PlanCategory category = PlanCategory::Functional;
  std::string language;
  std::string tier;
  bool held_out = false;
  std::size_t runs = 0;
  double run_completion_rate = 0.0;        // repeats that reached the DoD
  double milestone_completion_rate = 0.0;  // done milestones / total
  double hidden_test_pass_rate = 0.0;
  double mean_iterations = 0.0;
  double mean_cost_usd = 0.0;
  double cost_per_milestone_usd = 0.0;
  double escalation_rate = 0.0;  // repeats with at least one escalation
  std::vector<std::string> escalation_reasons;
  double mean_duration_s = 0.0;
  double security_pass_rate = 1.0;
  double recovery_rate = 1.0;
};

struct StratumScore {
  std::string name;
  std::size_t plans = 0;
  std::size_t runs = 0;
  Interval milestone_completion;
  Interval run_completion;
  Interval hidden_test_pass;
  Interval escalation_rate;
  Interval cost_per_milestone_usd;
  Interval security_pass;
  Interval recovery;
};

struct TargetResult {
  std::string name;
  bool applicable = true;
  double target = 0.0;
  double observed = 0.0;
  bool met = false;
};

struct SuiteScore {
  std::string suite_id;
  std::string config;
  std::vector<PlanScore> plans;
  StratumScore overall;
  std::vector<StratumScore> by_category;
  std::vector<StratumScore> by_language;
  std::vector<StratumScore> by_tier;
  StratumScore tuning;    // plans the prompts may be tuned on
  StratumScore held_out;  // plans never used while tuning
  std::vector<TargetResult> targets;
};

Result<SuiteScore> score_suite(const SuiteManifest& manifest, const OutcomeSet& outcomes);

// Executes every plan `manifest.repeats` times through the injected executor
// (the real one drives the run pipeline; tests inject a fake).
class PlanExecutor {
 public:
  virtual ~PlanExecutor() = default;
  virtual Result<PlanOutcome> run(const PlanEntry& plan, int repeat) = 0;
};

Result<SuiteScore> run_suite(const SuiteManifest& manifest, PlanExecutor& executor,
                             OutcomeSet* outcomes_out = nullptr);

}  // namespace cf::eval
