#include "cf/eval/outcome.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cf::eval {
namespace {

using nlohmann::json;

json outcome_to_json(const PlanOutcome& outcome) {
  return json{{"plan_id", outcome.plan_id},
              {"repeat", outcome.repeat},
              {"terminal", outcome.terminal},
              {"milestones_total", outcome.milestones_total},
              {"milestones_done", outcome.milestones_done},
              {"hidden_tests_passed", outcome.hidden_tests_passed},
              {"hidden_tests_total", outcome.hidden_tests_total},
              {"iterations", outcome.iterations},
              {"cost_usd", outcome.cost_usd},
              {"escalations", outcome.escalations},
              {"escalation_reasons", outcome.escalation_reasons},
              {"duration_s", outcome.duration_s},
              {"security_passed", outcome.security_passed},
              {"recovery_ok", outcome.recovery_ok}};
}

Result<PlanOutcome> outcome_from_json(const json& node) {
  PlanOutcome outcome;
  try {
    outcome.plan_id = node.at("plan_id").get<std::string>();
    outcome.repeat = node.value("repeat", 0);
    outcome.terminal = node.value("terminal", true);
    outcome.milestones_total = node.value("milestones_total", 0);
    outcome.milestones_done = node.value("milestones_done", 0);
    outcome.hidden_tests_passed = node.value("hidden_tests_passed", 0);
    outcome.hidden_tests_total = node.value("hidden_tests_total", 0);
    outcome.iterations = node.value("iterations", 0);
    outcome.cost_usd = node.value("cost_usd", 0.0);
    outcome.escalations = node.value("escalations", 0);
    outcome.escalation_reasons =
        node.value("escalation_reasons", std::vector<std::string>{});
    outcome.duration_s = node.value("duration_s", 0.0);
    outcome.security_passed = node.value("security_passed", true);
    outcome.recovery_ok = node.value("recovery_ok", true);
  } catch (const json::exception& error) {
    return fail(error_codes::kOutcomeInvalid,
                std::string("outcome record: ") + error.what());
  }
  if (outcome.plan_id.empty()) {
    return fail(error_codes::kOutcomeInvalid, "outcome record needs a plan_id");
  }
  if (outcome.milestones_done < 0 || outcome.milestones_total < 0 ||
      outcome.milestones_done > outcome.milestones_total) {
    return fail(error_codes::kOutcomeInvalid,
                "outcome for '" + outcome.plan_id + "' has inconsistent milestone counts");
  }
  if (outcome.hidden_tests_passed < 0 || outcome.hidden_tests_total < 0 ||
      outcome.hidden_tests_passed > outcome.hidden_tests_total) {
    return fail(error_codes::kOutcomeInvalid,
                "outcome for '" + outcome.plan_id + "' has inconsistent hidden test counts");
  }
  return outcome;
}

}  // namespace

Result<OutcomeSet> parse_outcomes(std::string_view text) {
  const json root = json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return fail(error_codes::kOutcomeInvalid, "outcomes must be a JSON object");
  }
  OutcomeSet set;
  set.suite_id = root.value("suite", std::string{});
  set.config = root.value("config", std::string{});
  if (set.suite_id.empty()) {
    return fail(error_codes::kOutcomeInvalid, "outcomes need a suite id");
  }
  const auto outcomes = root.find("outcomes");
  if (outcomes == root.end() || !outcomes->is_array()) {
    return fail(error_codes::kOutcomeInvalid, "outcomes need an 'outcomes' array");
  }
  for (const auto& node : *outcomes) {
    auto outcome = outcome_from_json(node);
    if (!outcome.has_value()) {
      return fail(outcome.error().code, outcome.error().message);
    }
    set.outcomes.push_back(std::move(*outcome));
  }
  return set;
}

Result<OutcomeSet> load_outcomes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kOutcomeInvalid,
                "cannot read outcomes file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_outcomes(buffer.str());
}

std::string outcomes_to_json(const OutcomeSet& set) {
  json root;
  root["suite"] = set.suite_id;
  root["config"] = set.config;
  root["outcomes"] = json::array();
  for (const auto& outcome : set.outcomes) {
    root["outcomes"].push_back(outcome_to_json(outcome));
  }
  return root.dump(2) + "\n";
}

}  // namespace cf::eval
