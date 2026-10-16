#include "cf/eval/report.hpp"

#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cf::eval {
namespace {

using nlohmann::json;

json interval_to_json(const Interval& interval) {
  return json{{"mean", interval.mean},
              {"low", interval.low},
              {"high", interval.high},
              {"samples", interval.samples}};
}

json stratum_to_json(const StratumScore& stratum) {
  return json{{"name", stratum.name},
              {"plans", stratum.plans},
              {"runs", stratum.runs},
              {"milestone_completion", interval_to_json(stratum.milestone_completion)},
              {"run_completion", interval_to_json(stratum.run_completion)},
              {"hidden_test_pass", interval_to_json(stratum.hidden_test_pass)},
              {"escalation_rate", interval_to_json(stratum.escalation_rate)},
              {"cost_per_milestone_usd", interval_to_json(stratum.cost_per_milestone_usd)},
              {"security_pass", interval_to_json(stratum.security_pass)},
              {"recovery", interval_to_json(stratum.recovery)}};
}

std::string percent(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << value * 100.0 << "%";
  return out.str();
}

std::string money(double value) {
  std::ostringstream out;
  out << "$" << std::fixed << std::setprecision(2) << value;
  return out.str();
}

std::string interval_text(const Interval& interval, bool as_percent) {
  std::ostringstream out;
  const auto render = [&](double value) {
    return as_percent ? percent(value) : money(value);
  };
  out << render(interval.mean);
  if (interval.samples > 1) {
    out << " [" << render(interval.low) << ", " << render(interval.high) << "]";
  }
  return out.str();
}

void stratum_row(std::ostringstream& out, const StratumScore& stratum) {
  out << "| " << stratum.name << " | " << stratum.plans << " | " << stratum.runs << " | "
      << interval_text(stratum.milestone_completion, true) << " | "
      << interval_text(stratum.hidden_test_pass, true) << " | "
      << interval_text(stratum.escalation_rate, true) << " | "
      << interval_text(stratum.cost_per_milestone_usd, false) << " | "
      << interval_text(stratum.security_pass, true) << " | "
      << interval_text(stratum.recovery, true) << " |\n";
}

}  // namespace

std::string score_to_json(const SuiteScore& score) {
  json root;
  root["suite"] = score.suite_id;
  root["config"] = score.config;
  root["plans"] = json::array();
  for (const auto& plan : score.plans) {
    root["plans"].push_back(
        json{{"plan_id", plan.plan_id},
             {"category", to_string(plan.category)},
             {"language", plan.language},
             {"tier", plan.tier},
             {"held_out", plan.held_out},
             {"runs", plan.runs},
             {"run_completion_rate", plan.run_completion_rate},
             {"milestone_completion_rate", plan.milestone_completion_rate},
             {"hidden_test_pass_rate", plan.hidden_test_pass_rate},
             {"mean_iterations", plan.mean_iterations},
             {"mean_cost_usd", plan.mean_cost_usd},
             {"cost_per_milestone_usd", plan.cost_per_milestone_usd},
             {"escalation_rate", plan.escalation_rate},
             {"escalation_reasons", plan.escalation_reasons},
             {"mean_duration_s", plan.mean_duration_s},
             {"security_pass_rate", plan.security_pass_rate},
             {"recovery_rate", plan.recovery_rate}});
  }
  root["overall"] = stratum_to_json(score.overall);
  root["by_category"] = json::array();
  for (const auto& stratum : score.by_category) {
    root["by_category"].push_back(stratum_to_json(stratum));
  }
  root["by_language"] = json::array();
  for (const auto& stratum : score.by_language) {
    root["by_language"].push_back(stratum_to_json(stratum));
  }
  root["by_tier"] = json::array();
  for (const auto& stratum : score.by_tier) {
    root["by_tier"].push_back(stratum_to_json(stratum));
  }
  root["tuning"] = stratum_to_json(score.tuning);
  root["held_out"] = stratum_to_json(score.held_out);
  root["targets"] = json::array();
  for (const auto& target : score.targets) {
    root["targets"].push_back(json{{"name", target.name},
                                   {"applicable", target.applicable},
                                   {"target", target.target},
                                   {"observed", target.observed},
                                   {"met", target.met}});
  }
  return root.dump(2) + "\n";
}

std::string score_to_markdown(const SuiteScore& score) {
  std::ostringstream out;
  out << "# Evaluation report: " << score.suite_id << "\n\n";
  if (!score.config.empty()) {
    out << "Configuration: `" << score.config << "`\n\n";
  }
  out << "## Targets\n\n";
  out << "| Target | Threshold | Observed | Result |\n|---|---|---|---|\n";
  for (const auto& target : score.targets) {
    out << "| " << target.name << " | " << percent(target.target) << " | ";
    if (target.applicable) {
      out << percent(target.observed) << " | " << (target.met ? "met" : "**missed**");
    } else {
      out << "n/a | not applicable";
    }
    out << " |\n";
  }
  out << "\n## Strata\n\n";
  out << "| Stratum | Plans | Runs | Milestone completion | Hidden tests | Escalations | "
         "Cost/milestone | Security | Recovery |\n";
  out << "|---|---|---|---|---|---|---|---|---|\n";
  stratum_row(out, score.overall);
  for (const auto& stratum : score.by_category) {
    stratum_row(out, stratum);
  }
  for (const auto& stratum : score.by_language) {
    stratum_row(out, stratum);
  }
  for (const auto& stratum : score.by_tier) {
    stratum_row(out, stratum);
  }
  stratum_row(out, score.tuning);
  stratum_row(out, score.held_out);
  out << "\nIntervals are mean ± 1.96·SE over the pooled per-repeat samples.\n";

  out << "\n## Plans\n\n";
  out << "| Plan | Category | Language | Tier | Held-out | Runs | Milestone completion | "
         "Hidden tests | Escalations | Cost/milestone | Iterations | Duration |\n";
  out << "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& plan : score.plans) {
    out << "| " << plan.plan_id << " | " << to_string(plan.category) << " | "
        << (plan.language.empty() ? "-" : plan.language) << " | "
        << (plan.tier.empty() ? "-" : plan.tier) << " | " << (plan.held_out ? "yes" : "no")
        << " | " << plan.runs << " | " << percent(plan.milestone_completion_rate) << " | "
        << percent(plan.hidden_test_pass_rate) << " | " << percent(plan.escalation_rate)
        << " | " << money(plan.cost_per_milestone_usd) << " | " << plan.mean_iterations
        << " | " << static_cast<int>(plan.mean_duration_s) << "s |\n";
  }

  bool any_reasons = false;
  for (const auto& plan : score.plans) {
    any_reasons = any_reasons || !plan.escalation_reasons.empty();
  }
  if (any_reasons) {
    out << "\n## Escalation reasons\n\n";
    for (const auto& plan : score.plans) {
      if (plan.escalation_reasons.empty()) {
        continue;
      }
      out << "- **" << plan.plan_id << "**: ";
      for (std::size_t i = 0; i < plan.escalation_reasons.size(); ++i) {
        out << (i == 0 ? "" : ", ") << plan.escalation_reasons[i];
      }
      out << "\n";
    }
  }
  return out.str();
}

}  // namespace cf::eval
