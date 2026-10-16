#include "cf/eval/suite.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "cf/plan/plan.hpp"

namespace cf::eval {
namespace {

Result<SuiteManifest> validate(SuiteManifest manifest) {
  if (manifest.suite_id.empty()) {
    return fail(error_codes::kSuiteInvalid, "suite id is required");
  }
  if (manifest.repeats < 1) {
    return fail(error_codes::kSuiteInvalid, "repeats must be at least 1");
  }
  for (const auto& [name, value] :
       {std::pair<std::string, double>{"completion", manifest.targets.completion},
        std::pair<std::string, double>{"security", manifest.targets.security},
        std::pair<std::string, double>{"recovery", manifest.targets.recovery}}) {
    if (value < 0.0 || value > 1.0) {
      return fail(error_codes::kSuiteInvalid, "target '" + name + "' must be in [0, 1]");
    }
  }
  if (manifest.plans.empty()) {
    return fail(error_codes::kSuiteInvalid, "the suite has no plans");
  }
  std::set<std::string> ids;
  for (const auto& plan : manifest.plans) {
    if (plan.id.empty()) {
      return fail(error_codes::kSuiteInvalid, "every plan needs an id");
    }
    if (!ids.insert(plan.id).second) {
      return fail(error_codes::kSuiteInvalid, "duplicate plan id '" + plan.id + "'");
    }
    if (plan.plan_path.empty()) {
      return fail(error_codes::kSuiteInvalid, "plan '" + plan.id + "' has no plan file");
    }
    if (plan.category == PlanCategory::Functional) {
      if (plan.tier.empty() || plan.language.empty()) {
        return fail(error_codes::kSuiteInvalid,
                    "functional plan '" + plan.id + "' needs a tier and a language");
      }
      if (plan.tier != "T1" && plan.tier != "T2" && plan.tier != "T3" &&
          plan.tier != "T4") {
        return fail(error_codes::kSuiteInvalid,
                    "plan '" + plan.id + "' has unknown tier '" + plan.tier + "'");
      }
    }
  }
  return manifest;
}

}  // namespace

std::string to_string(PlanCategory category) {
  switch (category) {
    case PlanCategory::Functional:
      return "functional";
    case PlanCategory::Reliability:
      return "reliability";
    case PlanCategory::Security:
      return "security";
    case PlanCategory::Governance:
      return "governance";
    case PlanCategory::Conflict:
      return "conflict";
  }
  return "functional";
}

std::optional<PlanCategory> category_from_string(std::string_view text) {
  if (text == "functional") {
    return PlanCategory::Functional;
  }
  if (text == "reliability") {
    return PlanCategory::Reliability;
  }
  if (text == "security") {
    return PlanCategory::Security;
  }
  if (text == "governance") {
    return PlanCategory::Governance;
  }
  if (text == "conflict") {
    return PlanCategory::Conflict;
  }
  return std::nullopt;
}

Result<SuiteManifest> parse_suite_manifest(std::string_view yaml,
                                           const std::filesystem::path& base_dir) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(yaml));
  } catch (const YAML::Exception& error) {
    return fail(error_codes::kSuiteParseFailed,
                std::string("suite yaml: ") + error.what());
  }
  if (!root.IsMap()) {
    return fail(error_codes::kSuiteParseFailed, "suite must be a mapping");
  }

  SuiteManifest manifest;
  manifest.base_dir = base_dir;
  manifest.suite_id = root["suite"].as<std::string>("");
  manifest.repeats = root["repeats"].as<int>(3);
  if (const auto targets = root["targets"]) {
    manifest.targets.completion = targets["completion"].as<double>(manifest.targets.completion);
    manifest.targets.security = targets["security"].as<double>(manifest.targets.security);
    manifest.targets.recovery = targets["recovery"].as<double>(manifest.targets.recovery);
  }

  const auto plans = root["plans"];
  if (!plans || !plans.IsSequence()) {
    return fail(error_codes::kSuiteParseFailed, "suite needs a 'plans' sequence");
  }
  for (const auto& node : plans) {
    PlanEntry entry;
    entry.id = node["id"].as<std::string>("");
    const std::string category = node["category"].as<std::string>("functional");
    const auto parsed = category_from_string(category);
    if (!parsed.has_value()) {
      return fail(error_codes::kSuiteParseFailed,
                  "plan '" + entry.id + "' has unknown category '" + category + "'");
    }
    entry.category = *parsed;
    entry.language = node["language"].as<std::string>("");
    entry.tier = node["tier"].as<std::string>("");
    entry.plan_path = node["plan"].as<std::string>("");
    entry.held_out = node["held_out"].as<bool>(false);
    manifest.plans.push_back(std::move(entry));
  }
  return validate(std::move(manifest));
}

Result<SuiteManifest> load_suite_manifest(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kSuiteParseFailed,
                "cannot read suite manifest: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_suite_manifest(buffer.str(), path.parent_path());
}

Result<std::vector<std::string>> validate_suite_plans(const SuiteManifest& manifest) {
  std::vector<std::string> plan_ids;
  for (const auto& entry : manifest.plans) {
    const std::filesystem::path path = manifest.base_dir / entry.plan_path;
    auto plan = cf::plan::parse_plan_file(path);
    if (!plan.has_value()) {
      return fail(error_codes::kSuiteInvalid,
                  "plan '" + entry.id + "': " + plan.error().message);
    }
    plan_ids.push_back(plan->plan_id);
  }
  return plan_ids;
}

}  // namespace cf::eval
