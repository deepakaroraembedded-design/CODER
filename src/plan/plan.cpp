#include "cf/plan/plan.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace cf::plan {
namespace {

// Internal validation failure; converted to cf::Error at the boundary.
struct PlanError : std::runtime_error {
  PlanError(const char* code_in, const std::string& message)
      : std::runtime_error(message), code(code_in) {}
  const char* code;
};

[[noreturn]] void schema_error(const std::string& message) {
  throw PlanError(error_codes::kSchemaInvalid, message);
}

[[noreturn]] void dependency_error(const std::string& message) {
  throw PlanError(error_codes::kDependencyInvalid, message);
}

std::string as_string(const YAML::Node& node, const std::string& context) {
  if (!node.IsScalar()) {
    schema_error(context + " must be a scalar");
  }
  try {
    return node.as<std::string>();
  } catch (const YAML::Exception& e) {
    schema_error(context + ": " + e.what());
  }
}

std::string required_string(const YAML::Node& map, const char* key,
                            const std::string& context) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    schema_error(context + " is missing required field '" + key + "'");
  }
  std::string value = as_string(node, context + "." + key);
  if (value.empty()) {
    schema_error(context + "." + key + " must not be empty");
  }
  return value;
}

std::optional<std::string> optional_string(const YAML::Node& map, const char* key,
                                           const std::string& context) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    return std::nullopt;
  }
  return as_string(node, context + "." + key);
}

std::vector<std::string> string_list(const YAML::Node& node,
                                     const std::string& context) {
  std::vector<std::string> out;
  if (!node || node.IsNull()) {
    return out;
  }
  if (!node.IsSequence()) {
    schema_error(context + " must be a sequence");
  }
  for (std::size_t i = 0; i < node.size(); ++i) {
    out.push_back(as_string(node[i], context + "[" + std::to_string(i) + "]"));
  }
  return out;
}

double required_double(const YAML::Node& node, const std::string& context) {
  if (!node.IsScalar()) {
    schema_error(context + " must be a number");
  }
  try {
    return node.as<double>();
  } catch (const YAML::Exception& e) {
    schema_error(context + ": " + e.what());
  }
}

// Pure YAML, or Markdown with a leading YAML front matter block.
std::string extract_yaml(std::string_view text) {
  std::size_t pos = 0;
  const std::size_t first_end = text.find('\n');
  std::string_view first_line =
      first_end == std::string_view::npos ? text : text.substr(0, first_end);
  if (!first_line.empty() && first_line.back() == '\r') {
    first_line.remove_suffix(1);
  }
  if (first_line != "---") {
    return std::string(text);
  }
  pos = first_end == std::string_view::npos ? text.size() : first_end + 1;
  while (pos < text.size()) {
    const std::size_t line_end = text.find('\n', pos);
    std::string_view line = line_end == std::string_view::npos
                                ? text.substr(pos)
                                : text.substr(pos, line_end - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line == "---" || line == "...") {
      return std::string(text.substr(0, pos));  // includes the opening '---'
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    pos = line_end + 1;
  }
  throw PlanError(error_codes::kParseFailed, "front matter is not terminated by '---'");
}

std::vector<std::string> topo_order(const std::vector<MilestoneSpec>& milestones) {
  std::map<std::string, std::size_t, std::less<>> index;
  for (std::size_t i = 0; i < milestones.size(); ++i) {
    index.emplace(milestones[i].id, i);
  }
  std::vector<std::size_t> in_degree(milestones.size(), 0);
  std::vector<std::vector<std::size_t>> dependents(milestones.size());
  for (std::size_t i = 0; i < milestones.size(); ++i) {
    for (const auto& dependency : milestones[i].depends_on) {
      const auto it = index.find(dependency);
      if (it == index.end()) {
        continue;  // reported by validation; keep the helper total
      }
      ++in_degree[i];
      dependents[it->second].push_back(i);
    }
  }
  std::vector<std::string> order;
  order.reserve(milestones.size());
  // Stable: among ready milestones, always take the earliest in input order.
  bool progress = true;
  std::vector<bool> done(milestones.size(), false);
  while (progress && order.size() < milestones.size()) {
    progress = false;
    for (std::size_t i = 0; i < milestones.size(); ++i) {
      if (done[i] || in_degree[i] != 0) {
        continue;
      }
      done[i] = true;
      order.push_back(milestones[i].id);
      for (const std::size_t dependent : dependents[i]) {
        --in_degree[dependent];
      }
      progress = true;
    }
  }
  return order;
}

void validate_milestones(const std::vector<MilestoneSpec>& milestones) {
  if (milestones.empty()) {
    schema_error("plan must declare at least one milestone");
  }
  std::set<std::string> ids;
  for (const auto& milestone : milestones) {
    if (!ids.insert(milestone.id).second) {
      dependency_error("duplicate milestone id '" + milestone.id + "'");
    }
  }
  for (const auto& milestone : milestones) {
    for (const auto& dependency : milestone.depends_on) {
      if (ids.find(dependency) == ids.end()) {
        dependency_error("milestone '" + milestone.id +
                         "' depends on unknown milestone '" + dependency + "'");
      }
    }
  }
  const std::vector<std::string> order = topo_order(milestones);
  if (order.size() != milestones.size()) {
    std::string remaining;
    for (const auto& milestone : milestones) {
      if (std::find(order.begin(), order.end(), milestone.id) == order.end()) {
        if (!remaining.empty()) {
          remaining += ", ";
        }
        remaining += milestone.id;
      }
    }
    dependency_error("milestone dependency cycle among: " + remaining);
  }
}

Plan parse_yaml(const std::string& yaml_text, const ValidationOptions& options,
                const std::string& source) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw PlanError(error_codes::kParseFailed,
                    "cannot parse plan YAML: " + std::string(e.what()));
  }
  if (!root || !root.IsMap()) {
    schema_error("plan must be a YAML mapping");
  }

  Plan plan;
  plan.source_path = source;
  plan.plan_id = required_string(root, "plan_id", "plan");
  plan.product = required_string(root, "product", "plan");
  plan.version = required_string(root, "version", "plan");
  if (const auto ref = optional_string(root, "requirements_ref", "plan")) {
    plan.requirements_ref = *ref;
  }

  const YAML::Node target = root["target"];
  if (!target || target.IsNull()) {
    schema_error("plan is missing required field 'target'");
  }
  if (!target.IsMap()) {
    schema_error("plan.target must be a mapping");
  }
  plan.target.language = required_string(target, "language", "target");
  plan.target.standard = required_string(target, "standard", "target");
  plan.target.build_system = required_string(target, "build_system", "target");
  plan.target.test_framework = required_string(target, "test_framework", "target");
  plan.target.platforms = string_list(target["platforms"], "target.platforms");

  const YAML::Node milestones = root["milestones"];
  if (!milestones || milestones.IsNull()) {
    schema_error("plan is missing required field 'milestones'");
  }
  if (!milestones.IsSequence()) {
    schema_error("plan.milestones must be a sequence");
  }
  for (std::size_t i = 0; i < milestones.size(); ++i) {
    const YAML::Node node = milestones[i];
    const std::string context = "milestones[" + std::to_string(i) + "]";
    if (!node.IsMap()) {
      schema_error(context + " must be a mapping");
    }
    MilestoneSpec spec;
    spec.id = required_string(node, "id", context);
    spec.title = required_string(node, "title", context);
    spec.requirements = string_list(node["requirements"], context + ".requirements");
    spec.depends_on = string_list(node["depends_on"], context + ".depends_on");
    spec.acceptance = string_list(node["acceptance"], context + ".acceptance");
    spec.writes = string_list(node["writes"], context + ".writes");
    spec.uses = string_list(node["uses"], context + ".uses");
    spec.writes_interfaces =
        string_list(node["writes_interfaces"], context + ".writes_interfaces");
    plan.milestones.push_back(std::move(spec));
  }
  validate_milestones(plan.milestones);

  if (const YAML::Node constraints = root["constraints"]) {
    if (!constraints.IsMap()) {
      schema_error("plan.constraints must be a mapping");
    }
    plan.forbidden = string_list(constraints["forbidden"], "constraints.forbidden");
  }

  if (const YAML::Node models = root["models"]) {
    if (!models.IsMap()) {
      schema_error("plan.models must be a mapping");
    }
    static const std::set<std::string> kRoles = {"planner", "coder", "reviewer", "qa"};
    for (const auto& entry : models) {
      const std::string role = as_string(entry.first, "models.<role>");
      if (kRoles.find(role) == kRoles.end()) {
        schema_error("unknown model role '" + role +
                     "' (expected planner, coder, reviewer or qa)");
      }
      plan.role_models[role] = as_string(entry.second, "models." + role);
    }
  }

  if (const auto inference = optional_string(root, "inference", "plan")) {
    if (*inference != "fireworks" && *inference != "local-vllm") {
      schema_error("inference must be 'fireworks' or 'local-vllm', got '" +
                   *inference + "'");
    }
    plan.inference = *inference;
  }

  if (const YAML::Node budget = root["budget"]) {
    if (!budget.IsMap()) {
      schema_error("plan.budget must be a mapping");
    }
    if (const YAML::Node usd = budget["max_usd_total"]) {
      plan.budget.max_usd_total = required_double(usd, "budget.max_usd_total");
    }
    if (const YAML::Node tokens = budget["max_tokens_total"]) {
      plan.budget.max_tokens_total =
          static_cast<uint64_t>(required_double(tokens, "budget.max_tokens_total"));
    }
    if (const YAML::Node iterations = budget["max_iterations_per_milestone"]) {
      plan.budget.max_iterations_per_milestone = static_cast<uint32_t>(
          required_double(iterations, "budget.max_iterations_per_milestone"));
    }
    if (const YAML::Node attempts = budget["max_attempts_per_defect"]) {
      plan.budget.max_attempts_per_defect =
          static_cast<uint32_t>(required_double(attempts, "budget.max_attempts_per_defect"));
    }
    if (const YAML::Node wall = budget["max_wall_clock_hours"]) {
      plan.budget.max_wall_clock_hours =
          required_double(wall, "budget.max_wall_clock_hours");
    }
  }

  // FR-18: the target must resolve to a toolchain profile.
  if (options.profile_exists && !options.profile_exists(plan.target)) {
    throw PlanError(error_codes::kNoToolchainProfile,
                    "no toolchain profile for " + plan.target.language + "/" +
                        plan.target.build_system + "/" + plan.target.test_framework);
  }
  return plan;
}

}  // namespace

const MilestoneSpec* Plan::find_milestone(std::string_view id) const {
  for (const auto& milestone : milestones) {
    if (milestone.id == id) {
      return &milestone;
    }
  }
  return nullptr;
}

std::vector<std::string> Plan::topological_order() const {
  return topo_order(milestones);
}

Result<Plan> parse_plan_string(std::string_view text,
                               const ValidationOptions& options) {
  try {
    return parse_yaml(extract_yaml(text), options, "<string>");
  } catch (const PlanError& e) {
    return fail(e.code, e.what());
  } catch (const std::exception& e) {
    return fail(error_codes::kParseFailed, e.what());
  }
}

Result<Plan> parse_plan_file(const std::filesystem::path& path,
                             const ValidationOptions& options) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kParseFailed, "cannot open plan file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return fail(error_codes::kParseFailed, "cannot read plan file: " + path.string());
  }
  try {
    return parse_yaml(extract_yaml(buffer.str()), options, path.string());
  } catch (const PlanError& e) {
    return fail(e.code, e.what());
  } catch (const std::exception& e) {
    return fail(error_codes::kParseFailed, e.what());
  }
}

}  // namespace cf::plan
