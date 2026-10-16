#include "cf/eval/score.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace cf::eval {
namespace {

Interval interval_from(const std::vector<double>& samples) {
  Interval result;
  result.samples = samples.size();
  if (samples.empty()) {
    return result;
  }
  double sum = 0.0;
  for (const double value : samples) {
    sum += value;
  }
  result.mean = sum / static_cast<double>(samples.size());
  result.low = result.mean;
  result.high = result.mean;
  if (samples.size() > 1) {
    double squares = 0.0;
    for (const double value : samples) {
      squares += (value - result.mean) * (value - result.mean);
    }
    const double sd = std::sqrt(squares / static_cast<double>(samples.size() - 1));
    const double standard_error = sd / std::sqrt(static_cast<double>(samples.size()));
    result.low = result.mean - 1.96 * standard_error;
    result.high = result.mean + 1.96 * standard_error;
  }
  return result;
}

// One sample per repeat for every metric the report aggregates.
struct Samples {
  std::vector<double> milestone_completion;
  std::vector<double> run_completion;
  std::vector<double> hidden_test_pass;
  std::vector<double> escalation;
  std::vector<double> cost_per_milestone;
  std::vector<double> security;
  std::vector<double> recovery;
};

Samples samples_for(const std::vector<const PlanOutcome*>& outcomes) {
  Samples samples;
  for (const PlanOutcome* outcome : outcomes) {
    if (outcome->milestones_total > 0) {
      const double done = static_cast<double>(outcome->milestones_done);
      const double total = static_cast<double>(outcome->milestones_total);
      samples.milestone_completion.push_back(done / total);
      samples.run_completion.push_back(
          outcome->terminal && outcome->milestones_done == outcome->milestones_total ? 1.0
                                                                                     : 0.0);
      if (outcome->milestones_done > 0) {
        samples.cost_per_milestone.push_back(outcome->cost_usd / done);
      }
    }
    if (outcome->hidden_tests_total > 0) {
      samples.hidden_test_pass.push_back(
          static_cast<double>(outcome->hidden_tests_passed) /
          static_cast<double>(outcome->hidden_tests_total));
    }
    samples.escalation.push_back(outcome->escalations > 0 ? 1.0 : 0.0);
    samples.security.push_back(outcome->security_passed ? 1.0 : 0.0);
    samples.recovery.push_back(outcome->recovery_ok ? 1.0 : 0.0);
  }
  return samples;
}

StratumScore stratum_from(const std::string& name, std::size_t plans,
                          const std::vector<const PlanOutcome*>& outcomes) {
  const Samples samples = samples_for(outcomes);
  StratumScore stratum;
  stratum.name = name;
  stratum.plans = plans;
  stratum.runs = outcomes.size();
  stratum.milestone_completion = interval_from(samples.milestone_completion);
  stratum.run_completion = interval_from(samples.run_completion);
  stratum.hidden_test_pass = interval_from(samples.hidden_test_pass);
  stratum.escalation_rate = interval_from(samples.escalation);
  stratum.cost_per_milestone_usd = interval_from(samples.cost_per_milestone);
  stratum.security_pass = interval_from(samples.security);
  stratum.recovery = interval_from(samples.recovery);
  return stratum;
}

PlanScore plan_score(const PlanEntry& entry, const std::vector<const PlanOutcome*>& outcomes) {
  const Samples samples = samples_for(outcomes);
  PlanScore score;
  score.plan_id = entry.id;
  score.category = entry.category;
  score.language = entry.language;
  score.tier = entry.tier;
  score.held_out = entry.held_out;
  score.runs = outcomes.size();
  score.milestone_completion_rate = interval_from(samples.milestone_completion).mean;
  score.run_completion_rate = interval_from(samples.run_completion).mean;
  score.hidden_test_pass_rate = interval_from(samples.hidden_test_pass).mean;
  score.escalation_rate = interval_from(samples.escalation).mean;
  score.cost_per_milestone_usd = interval_from(samples.cost_per_milestone).mean;
  score.security_pass_rate = interval_from(samples.security).mean;
  score.recovery_rate = interval_from(samples.recovery).mean;
  double iterations = 0.0;
  double cost = 0.0;
  double duration = 0.0;
  std::set<std::string> reasons;
  for (const PlanOutcome* outcome : outcomes) {
    iterations += outcome->iterations;
    cost += outcome->cost_usd;
    duration += outcome->duration_s;
    for (const auto& reason : outcome->escalation_reasons) {
      reasons.insert(reason);
    }
  }
  const double runs = static_cast<double>(outcomes.size());
  score.mean_iterations = iterations / runs;
  score.mean_cost_usd = cost / runs;
  score.mean_duration_s = duration / runs;
  score.escalation_reasons.assign(reasons.begin(), reasons.end());
  return score;
}

const std::vector<const PlanOutcome*>& outcomes_for(
    const std::map<std::string, std::vector<const PlanOutcome*>>& by_plan,
    const std::string& plan_id) {
  static const std::vector<const PlanOutcome*> empty;
  const auto found = by_plan.find(plan_id);
  return found == by_plan.end() ? empty : found->second;
}

}  // namespace

Result<SuiteScore> score_suite(const SuiteManifest& manifest, const OutcomeSet& outcomes) {
  if (!outcomes.suite_id.empty() && outcomes.suite_id != manifest.suite_id) {
    return fail(error_codes::kOutcomeInvalid,
                "outcomes are for suite '" + outcomes.suite_id + "', not '" +
                    manifest.suite_id + "'");
  }
  std::map<std::string, const PlanEntry*> entries;
  for (const auto& entry : manifest.plans) {
    entries.emplace(entry.id, &entry);
  }
  std::map<std::string, std::vector<const PlanOutcome*>> by_plan;
  for (const auto& outcome : outcomes.outcomes) {
    if (entries.find(outcome.plan_id) == entries.end()) {
      return fail(error_codes::kOutcomeInvalid,
                  "outcome references unknown plan '" + outcome.plan_id + "'");
    }
    by_plan[outcome.plan_id].push_back(&outcome);
  }

  SuiteScore score;
  score.suite_id = manifest.suite_id;
  score.config = outcomes.config;
  for (const auto& entry : manifest.plans) {
    const auto& plan_outcomes = outcomes_for(by_plan, entry.id);
    if (plan_outcomes.empty()) {
      continue;
    }
    score.plans.push_back(plan_score(entry, plan_outcomes));
  }

  std::vector<const PlanOutcome*> all;
  std::vector<const PlanOutcome*> tuning;
  std::vector<const PlanOutcome*> held_out;
  std::size_t tuning_plans = 0;
  std::size_t held_out_plans = 0;
  std::map<std::string, std::vector<const PlanOutcome*>> by_category;
  std::map<std::string, std::vector<const PlanOutcome*>> by_language;
  std::map<std::string, std::vector<const PlanOutcome*>> by_tier;
  std::map<std::string, std::size_t> category_plans;
  std::map<std::string, std::size_t> language_plans;
  std::map<std::string, std::size_t> tier_plans;
  for (const auto& entry : manifest.plans) {
    const auto& plan_outcomes = outcomes_for(by_plan, entry.id);
    if (plan_outcomes.empty()) {
      continue;
    }
    all.insert(all.end(), plan_outcomes.begin(), plan_outcomes.end());
    if (entry.held_out) {
      held_out.insert(held_out.end(), plan_outcomes.begin(), plan_outcomes.end());
      ++held_out_plans;
    } else {
      tuning.insert(tuning.end(), plan_outcomes.begin(), plan_outcomes.end());
      ++tuning_plans;
    }
    const std::string category = to_string(entry.category);
    by_category[category].insert(by_category[category].end(), plan_outcomes.begin(),
                                 plan_outcomes.end());
    ++category_plans[category];
    if (!entry.language.empty()) {
      by_language[entry.language].insert(by_language[entry.language].end(),
                                         plan_outcomes.begin(), plan_outcomes.end());
      ++language_plans[entry.language];
    }
    if (!entry.tier.empty()) {
      by_tier[entry.tier].insert(by_tier[entry.tier].end(), plan_outcomes.begin(),
                                 plan_outcomes.end());
      ++tier_plans[entry.tier];
    }
  }

  score.overall = stratum_from("overall", score.plans.size(), all);
  for (const auto& [name, plan_outcomes] : by_category) {
    score.by_category.push_back(
        stratum_from(name, category_plans[name], plan_outcomes));
  }
  for (const auto& [name, plan_outcomes] : by_language) {
    score.by_language.push_back(
        stratum_from(name, language_plans[name], plan_outcomes));
  }
  for (const auto& [name, plan_outcomes] : by_tier) {
    score.by_tier.push_back(stratum_from(name, tier_plans[name], plan_outcomes));
  }
  score.tuning = stratum_from("tuning", tuning_plans, tuning);
  score.held_out = stratum_from("held_out", held_out_plans, held_out);

  const auto category_stratum = [&score](const std::string& name) -> const StratumScore* {
    for (const auto& stratum : score.by_category) {
      if (stratum.name == name) {
        return &stratum;
      }
    }
    return nullptr;
  };

  TargetResult completion;
  completion.name = "completion";
  completion.target = manifest.targets.completion;
  if (const StratumScore* functional = category_stratum("functional")) {
    completion.observed = functional->milestone_completion.mean;
    completion.met = completion.observed >= completion.target;
  } else {
    completion.applicable = false;
    completion.met = true;
  }
  score.targets.push_back(completion);

  TargetResult security;
  security.name = "security";
  security.target = manifest.targets.security;
  if (const StratumScore* stratum = category_stratum("security")) {
    security.observed = stratum->security_pass.mean;
    security.met = security.observed >= security.target;
  } else {
    security.applicable = false;
    security.met = true;
  }
  score.targets.push_back(security);

  TargetResult recovery;
  recovery.name = "recovery";
  recovery.target = manifest.targets.recovery;
  if (const StratumScore* stratum = category_stratum("reliability")) {
    recovery.observed = stratum->recovery.mean;
    recovery.met = recovery.observed >= recovery.target;
  } else {
    recovery.applicable = false;
    recovery.met = true;
  }
  score.targets.push_back(recovery);

  return score;
}

Result<SuiteScore> run_suite(const SuiteManifest& manifest, PlanExecutor& executor,
                             OutcomeSet* outcomes_out) {
  OutcomeSet outcomes;
  outcomes.suite_id = manifest.suite_id;
  for (const auto& entry : manifest.plans) {
    for (int repeat = 0; repeat < manifest.repeats; ++repeat) {
      auto outcome = executor.run(entry, repeat);
      if (!outcome.has_value()) {
        return fail(outcome.error().code, outcome.error().message);
      }
      if (outcome->plan_id.empty()) {
        outcome->plan_id = entry.id;
      }
      if (outcome->plan_id != entry.id) {
        return fail(error_codes::kOutcomeInvalid,
                    "executor returned an outcome for '" + outcome->plan_id +
                        "' while running '" + entry.id + "'");
      }
      outcome->repeat = repeat;
      outcomes.outcomes.push_back(std::move(*outcome));
    }
  }
  if (outcomes_out != nullptr) {
    *outcomes_out = outcomes;
  }
  return score_suite(manifest, outcomes);
}

}  // namespace cf::eval
