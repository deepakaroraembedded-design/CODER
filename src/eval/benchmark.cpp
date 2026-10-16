#include "cf/eval/benchmark.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace cf::eval {
namespace {

using Json = nlohmann::json;

Result<BenchmarkConfig> validate(BenchmarkConfig config) {
  if (config.benchmark_id.empty()) {
    return fail(error_codes::kBenchmarkInvalid, "benchmark id is required");
  }
  if (config.repeats < 0) {
    return fail(error_codes::kBenchmarkInvalid, "repeats cannot be negative");
  }
  if (config.candidates.empty()) {
    return fail(error_codes::kBenchmarkInvalid, "the benchmark has no candidates");
  }
  std::set<std::string> names;
  for (const auto& candidate : config.candidates) {
    if (candidate.name.empty()) {
      return fail(error_codes::kBenchmarkInvalid, "every candidate needs a name");
    }
    if (!names.insert(candidate.name).second) {
      return fail(error_codes::kBenchmarkInvalid,
                  "duplicate candidate name '" + candidate.name + "'");
    }
    if (candidate.role_models.empty()) {
      return fail(error_codes::kBenchmarkInvalid,
                  "candidate '" + candidate.name + "' has no role models");
    }
    for (const auto& [role, model] : candidate.role_models) {
      if (model.empty()) {
        return fail(error_codes::kBenchmarkInvalid,
                    "candidate '" + candidate.name + "' has an empty model for role '" +
                        role + "'");
      }
    }
  }
  std::set<std::string> plans;
  for (const auto& plan : config.plans) {
    if (plan.empty()) {
      return fail(error_codes::kBenchmarkInvalid, "plan ids cannot be empty");
    }
    if (!plans.insert(plan).second) {
      return fail(error_codes::kBenchmarkInvalid, "duplicate plan id '" + plan + "'");
    }
  }
  return config;
}

std::string format_double(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(4) << value;
  return out.str();
}

}  // namespace

Result<BenchmarkConfig> parse_benchmark_config(std::string_view yaml,
                                               const std::filesystem::path& base_dir) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(yaml));
  } catch (const YAML::Exception& error) {
    return fail(error_codes::kBenchmarkParseFailed,
                std::string("benchmark yaml: ") + error.what());
  }
  if (!root.IsMap()) {
    return fail(error_codes::kBenchmarkParseFailed, "benchmark must be a mapping");
  }

  BenchmarkConfig config;
  config.base_dir = base_dir;
  config.benchmark_id = root["benchmark"].as<std::string>("");
  config.provider_base_url =
      root["provider_base_url"].as<std::string>(config.provider_base_url);
  config.api_key_env = root["api_key_env"].as<std::string>(config.api_key_env);
  config.repeats = root["repeats"].as<int>(0);
  if (const auto plans = root["plans"]) {
    if (!plans.IsSequence()) {
      return fail(error_codes::kBenchmarkParseFailed, "'plans' must be a sequence");
    }
    for (const auto& node : plans) {
      config.plans.push_back(node.as<std::string>(""));
    }
  }

  const auto candidates = root["candidates"];
  if (!candidates || !candidates.IsSequence()) {
    return fail(error_codes::kBenchmarkParseFailed,
                "benchmark needs a 'candidates' sequence");
  }
  for (const auto& node : candidates) {
    Candidate candidate;
    candidate.name = node["name"].as<std::string>("");
    const auto roles = node["roles"];
    if (roles) {
      if (!roles.IsMap()) {
        return fail(error_codes::kBenchmarkParseFailed,
                    "candidate '" + candidate.name + "': 'roles' must be a mapping");
      }
      for (const auto& entry : roles) {
        candidate.role_models[entry.first.as<std::string>("")] =
            entry.second.as<std::string>("");
      }
    }
    config.candidates.push_back(std::move(candidate));
  }

  if (const auto prices = root["prices"]) {
    if (!prices.IsMap()) {
      return fail(error_codes::kBenchmarkParseFailed, "'prices' must be a mapping");
    }
    for (const auto& entry : prices) {
      ModelPrice price;
      price.usd_per_mtok_in = entry.second["in"].as<double>(0.0);
      price.usd_per_mtok_out = entry.second["out"].as<double>(0.0);
      config.prices[entry.first.as<std::string>("")] = price;
    }
  }

  return validate(std::move(config));
}

Result<BenchmarkConfig> load_benchmark_config(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kBenchmarkParseFailed,
                "cannot read benchmark config: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_benchmark_config(buffer.str(), path.parent_path());
}

Result<SuiteManifest> restrict_suite(const SuiteManifest& manifest,
                                     const BenchmarkConfig& benchmark) {
  SuiteManifest restricted = manifest;
  restricted.plans.clear();
  if (benchmark.plans.empty()) {
    restricted.plans = manifest.plans;
  } else {
    for (const auto& id : benchmark.plans) {
      const auto found = std::find_if(
          manifest.plans.begin(), manifest.plans.end(),
          [&id](const PlanEntry& entry) { return entry.id == id; });
      if (found == manifest.plans.end()) {
        return fail(error_codes::kBenchmarkInvalid,
                    "benchmark lists unknown plan '" + id + "'");
      }
      restricted.plans.push_back(*found);
    }
  }
  if (benchmark.repeats > 0) {
    restricted.repeats = benchmark.repeats;
  }
  return restricted;
}

std::optional<std::size_t> recommend_candidate(
    const std::vector<CandidateResult>& results) {
  std::optional<std::size_t> best;
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (!best.has_value()) {
      best = index;
      continue;
    }
    const auto& current = results[index];
    const auto& incumbent = results[*best];
    const auto better = [&](double a, double b) { return a > b + 1e-9; };
    if (better(current.score.overall.run_completion.mean,
               incumbent.score.overall.run_completion.mean)) {
      best = index;
      continue;
    }
    if (better(incumbent.score.overall.run_completion.mean,
               current.score.overall.run_completion.mean)) {
      continue;
    }
    if (better(current.score.overall.milestone_completion.mean,
               incumbent.score.overall.milestone_completion.mean)) {
      best = index;
      continue;
    }
    if (better(incumbent.score.overall.milestone_completion.mean,
               current.score.overall.milestone_completion.mean)) {
      continue;
    }
    if (better(current.score.overall.hidden_test_pass.mean,
               incumbent.score.overall.hidden_test_pass.mean)) {
      best = index;
      continue;
    }
    if (better(incumbent.score.overall.hidden_test_pass.mean,
               current.score.overall.hidden_test_pass.mean)) {
      continue;
    }
    const double current_cost = current.score.overall.cost_per_milestone_usd.mean;
    const double incumbent_cost = incumbent.score.overall.cost_per_milestone_usd.mean;
    if (current_cost < incumbent_cost - 1e-9) {
      best = index;
    }
  }
  return best;
}

std::string benchmark_summary_to_json(const BenchmarkConfig& config,
                                      const std::vector<CandidateResult>& results,
                                      std::optional<std::size_t> winner) {
  Json root;
  root["benchmark"] = config.benchmark_id;
  root["provider_base_url"] = config.provider_base_url;
  root["plans"] = config.plans;
  Json candidates = Json::array();
  for (const auto& result : results) {
    Json row;
    row["name"] = result.name;
    row["config"] = result.outcomes.config;
    row["runs"] = result.score.overall.runs;
    row["run_completion"] = result.score.overall.run_completion.mean;
    row["milestone_completion"] = result.score.overall.milestone_completion.mean;
    row["hidden_test_pass"] = result.score.overall.hidden_test_pass.mean;
    row["escalation_rate"] = result.score.overall.escalation_rate.mean;
    row["cost_per_milestone_usd"] = result.score.overall.cost_per_milestone_usd.mean;
    candidates.push_back(std::move(row));
  }
  root["candidates"] = std::move(candidates);
  if (winner.has_value()) {
    root["recommended"] = results[*winner].name;
  }
  return root.dump(2) + "\n";
}

std::string benchmark_summary_to_markdown(const BenchmarkConfig& config,
                                          const std::vector<CandidateResult>& results,
                                          std::optional<std::size_t> winner) {
  std::ostringstream out;
  out << "# Model benchmark: " << config.benchmark_id << "\n\n";
  out << "| Candidate | Runs | Run completion | Milestone completion | Hidden tests | "
         "Escalations | Cost/milestone (USD) |\n";
  out << "|---|---|---|---|---|---|---|\n";
  for (const auto& result : results) {
    out << "| " << result.name << " | " << result.score.overall.runs << " | "
        << format_double(result.score.overall.run_completion.mean) << " | "
        << format_double(result.score.overall.milestone_completion.mean) << " | "
        << format_double(result.score.overall.hidden_test_pass.mean) << " | "
        << format_double(result.score.overall.escalation_rate.mean) << " | "
        << format_double(result.score.overall.cost_per_milestone_usd.mean) << " |\n";
  }
  out << "\n";
  if (winner.has_value()) {
    out << "Recommended: **" << results[*winner].name << "** (";
    const auto& roles = config.candidates.empty()
                            ? std::map<std::string, std::string, std::less<>>{}
                            : config.candidates[*winner].role_models;
    bool first = true;
    for (const auto& [role, model] : roles) {
      if (!first) {
        out << ", ";
      }
      out << role << "=" << model;
      first = false;
    }
    out << ").\n";
  } else {
    out << "No candidates were run.\n";
  }
  return out.str();
}

}  // namespace cf::eval
