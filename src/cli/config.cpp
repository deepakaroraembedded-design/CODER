#include "cf/cli/config.hpp"

#include <string_view>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace cf::cli {
namespace {

tl::unexpected<Error> invalid(std::string_view key, std::string_view why) {
  return fail(config_error_codes::kInvalid,
              "cf.yaml: " + std::string(key) + ": " + std::string(why));
}

tl::unexpected<Error> unknown_key(std::string_view key, std::string_view parent) {
  return fail(config_error_codes::kInvalid,
              "cf.yaml: unknown key '" + std::string(key) + "'" +
                  (parent.empty() ? "" : " in '" + std::string(parent) + "'"));
}

tl::unexpected<Error> unsupported(std::string_view key) {
  return fail(config_error_codes::kInvalid,
              "cf.yaml: '" + std::string(key) +
                  "' is in the design (section 15) but not supported by this "
                  "build yet");
}

// Every key of `node` must be in `allowed`; returns the offending key.
Status check_keys(const YAML::Node& node, std::string_view parent,
                  const std::vector<std::string_view>& allowed) {
  for (const auto& entry : node) {
    const std::string key = entry.first.as<std::string>();
    bool ok = false;
    for (const std::string_view allowed_key : allowed) {
      if (key == allowed_key) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      return unknown_key(key, parent);
    }
  }
  return {};
}

template <typename T>
Status read_into(const YAML::Node& node, std::string_view key, T& out) {
  if (!node) {
    return {};
  }
  try {
    out = node.as<T>();
  } catch (const YAML::Exception& e) {
    return invalid(key, e.what());
  }
  return {};
}

template <typename T>
Status read_optional(const YAML::Node& node, std::string_view key,
                     std::optional<T>& out) {
  if (!node) {
    return {};
  }
  try {
    out = node.as<T>();
  } catch (const YAML::Exception& e) {
    return invalid(key, e.what());
  }
  return {};
}

// Documented top-level sections this build does not consume yet.
const std::vector<std::string_view> kUnsupportedTopLevel = {
    "approvals", "rag", "reference_docs",
};

const std::vector<std::string_view> kTopLevel = {
    "run", "github", "providers", "models", "budget", "gates", "review",
    "sandbox",
};

}  // namespace

Result<FrameworkConfig> load_config(const std::filesystem::path& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::BadFile& e) {
    return fail(config_error_codes::kOpenFailed,
                path.string() + ": " + e.what());
  } catch (const YAML::Exception& e) {
    return fail(config_error_codes::kInvalid, path.string() + ": " + e.what());
  }
  if (root.IsNull()) {
    return FrameworkConfig{};
  }
  if (!root.IsMap()) {
    return invalid("cf.yaml", "the top level must be a mapping");
  }

  FrameworkConfig config;

  for (const auto& entry : root) {
    const std::string key = entry.first.as<std::string>();
    bool known = false;
    for (const std::string_view top : kTopLevel) {
      if (key == top) {
        known = true;
        break;
      }
    }
    if (!known) {
      for (const std::string_view top : kUnsupportedTopLevel) {
        if (key == top) {
          return unsupported(key);
        }
      }
      return unknown_key(key, "");
    }
  }

  if (const YAML::Node run = root["run"]) {
    auto checked = check_keys(run, "run", {"workspace", "repo", "base_branch"});
    if (!checked) {
      return tl::make_unexpected(checked.error());
    }
    if (auto status = read_optional(run["workspace"], "run.workspace",
                                    config.run.workspace);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(run["repo"], "run.repo", config.run.repo);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(run["base_branch"], "run.base_branch",
                                    config.run.base_branch);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  if (const YAML::Node github = root["github"]) {
    for (const auto& entry : github) {
      const std::string key = entry.first.as<std::string>();
      if (key == "app_id" || key == "installation_id" || key == "private_key_ref") {
        continue;
      }
      return unsupported("github." + key);
    }
    if (auto status = read_optional(github["app_id"], "github.app_id",
                                    config.github.app_id);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(github["installation_id"],
                                    "github.installation_id",
                                    config.github.installation_id);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(github["private_key_ref"],
                                    "github.private_key_ref",
                                    config.github.private_key_ref);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.github.private_key_ref.has_value() &&
        config.github.private_key_ref->find("secretmanager://") == 0) {
      return invalid("github.private_key_ref",
                     "secret manager references are not resolvable in this "
                     "build; use a PEM file path");
    }
  }

  if (const YAML::Node providers = root["providers"]) {
    if (!providers.IsMap()) {
      return invalid("providers", "must be a mapping of name to provider");
    }
    for (const auto& entry : providers) {
      const std::string name = entry.first.as<std::string>();
      const YAML::Node provider = entry.second;
      auto checked = check_keys(provider, "providers." + name,
                                {"base_url", "api_key_env"});
      if (!checked) {
        return tl::make_unexpected(checked.error());
      }
      FrameworkConfig::Provider parsed;
      if (const YAML::Node url = provider["base_url"]) {
        auto status = read_into(url, "providers." + name + ".base_url",
                                parsed.base_url);
        if (!status) {
          return tl::make_unexpected(status.error());
        }
      } else {
        return invalid("providers." + name, "base_url is required");
      }
      if (const YAML::Node env = provider["api_key_env"]) {
        auto status = read_into(env, "providers." + name + ".api_key_env",
                                parsed.api_key_env);
        if (!status) {
          return tl::make_unexpected(status.error());
        }
      } else {
        return invalid("providers." + name, "api_key_env is required");
      }
      config.providers[name] = std::move(parsed);
    }
  }

  if (const YAML::Node models = root["models"]) {
    auto checked = check_keys(models, "models", {"default_provider", "roles"});
    if (!checked) {
      return tl::make_unexpected(checked.error());
    }
    if (const YAML::Node provider = models["default_provider"]) {
      auto status = read_into(provider, "models.default_provider",
                              config.models.default_provider);
      if (!status) {
        return tl::make_unexpected(status.error());
      }
    }
    if (const YAML::Node roles = models["roles"]) {
      if (!roles.IsMap()) {
        return invalid("models.roles", "must be a mapping of role to model");
      }
      for (const auto& entry : roles) {
        const std::string role = entry.first.as<std::string>();
        if (role != "coder" && role != "reviewer" && role != "qa" &&
            role != "planner") {
          return invalid("models.roles." + role,
                         "unknown role (expected coder, reviewer, qa or planner)");
        }
        FrameworkConfig::RoleModel role_model;
        for (const auto& role_entry : entry.second) {
          const std::string key = role_entry.first.as<std::string>();
          if (key == "family") {
            auto status =
                read_into(role_entry.second, "models.roles." + role + ".family",
                          role_model.family);
            if (!status) {
              return tl::make_unexpected(status.error());
            }
          } else if (key == "temperature") {
            auto status = read_optional(
                role_entry.second, "models.roles." + role + ".temperature",
                role_model.temperature);
            if (!status) {
              return tl::make_unexpected(status.error());
            }
          } else if (key == "max_output_tokens") {
            auto status = read_optional(
                role_entry.second, "models.roles." + role + ".max_output_tokens",
                role_model.max_output_tokens);
            if (!status) {
              return tl::make_unexpected(status.error());
            }
          } else {
            // Any other key is a provider name mapping to its model id
            // (design section 15: fireworks / local-vllm / ...).
            std::string id;
            auto status =
                read_into(role_entry.second,
                          "models.roles." + role + "." + key, id);
            if (!status) {
              return tl::make_unexpected(status.error());
            }
            role_model.ids[key] = std::move(id);
          }
        }
        config.models.roles[role] = std::move(role_model);
      }
    }
  }

  if (const YAML::Node budget = root["budget"]) {
    for (const auto& entry : budget) {
      const std::string key = entry.first.as<std::string>();
      if (key == "max_usd_total" || key == "max_tokens_total" ||
          key == "max_iterations_per_milestone" ||
          key == "max_attempts_per_defect") {
        continue;
      }
      return unsupported("budget." + key);
    }
    if (auto status = read_optional(budget["max_usd_total"],
                                    "budget.max_usd_total",
                                    config.budget.max_usd_total);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(budget["max_tokens_total"],
                                    "budget.max_tokens_total",
                                    config.budget.max_tokens_total);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(budget["max_iterations_per_milestone"],
                                    "budget.max_iterations_per_milestone",
                                    config.budget.max_iterations_per_milestone);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(budget["max_attempts_per_defect"],
                                    "budget.max_attempts_per_defect",
                                    config.budget.max_attempts_per_defect);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  if (const YAML::Node gates = root["gates"]) {
    for (const auto& entry : gates) {
      const std::string key = entry.first.as<std::string>();
      if (key == "profiles_dir") {
        continue;
      }
      return unsupported("gates." + key);
    }
    if (auto status = read_optional(gates["profiles_dir"], "gates.profiles_dir",
                                    config.gates.profiles_dir);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  if (const YAML::Node review = root["review"]) {
    for (const auto& entry : review) {
      const std::string key = entry.first.as<std::string>();
      if (key == "diff_threshold_lines") {
        continue;
      }
      return unsupported("review." + key);
    }
    if (auto status = read_optional(review["diff_threshold_lines"],
                                    "review.diff_threshold_lines",
                                    config.review.diff_threshold_lines);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  if (const YAML::Node sandbox = root["sandbox"]) {
    for (const auto& entry : sandbox) {
      const std::string key = entry.first.as<std::string>();
      if (key == "cpu" || key == "memory_gb" || key == "job_timeout_min") {
        continue;
      }
      return unsupported("sandbox." + key);
    }
    if (auto status = read_optional(sandbox["cpu"], "sandbox.cpu",
                                    config.sandbox.cpu);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(sandbox["memory_gb"], "sandbox.memory_gb",
                                    config.sandbox.memory_gb);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(sandbox["job_timeout_min"],
                                    "sandbox.job_timeout_min",
                                    config.sandbox.job_timeout_min);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  return config;
}

Status apply_config(ExecuteOptions& options, const FrameworkConfig& config) {
  if (config.run.workspace.has_value()) {
    options.workspace = *config.run.workspace;
  }
  if (config.run.repo.has_value()) {
    // A local checkout path in this build (cloning is not implemented).
    options.repository_path = *config.run.repo;
  }
  if (config.run.base_branch.has_value()) {
    options.base_branch = *config.run.base_branch;
  }
  if (config.gates.profiles_dir.has_value()) {
    options.profiles_dir = *config.gates.profiles_dir;
  }
  if (config.github.app_id.has_value()) {
    options.github_app_id = *config.github.app_id;
  }
  if (config.github.installation_id.has_value()) {
    options.github_installation_id = *config.github.installation_id;
  }
  if (config.github.private_key_ref.has_value()) {
    options.github_private_key_file = *config.github.private_key_ref;
  }

  // Provider selection (design section 13.1: a run never mixes providers).
  if (!config.providers.empty()) {
    const auto provider = config.providers.find(config.models.default_provider);
    if (provider == config.providers.end()) {
      return invalid("models.default_provider",
                     "'" + config.models.default_provider +
                         "' has no entry in providers");
    }
    options.provider_base_url = provider->second.base_url;
    options.api_key_env = provider->second.api_key_env;
  }

  // Role models: when the config names any, all three agent roles must
  // resolve for the default provider (no mixing), replacing the built-in
  // pins; the planner feeds the design refiner.
  if (!config.models.roles.empty()) {
    for (const char* role : {"coder", "reviewer", "qa"}) {
      const auto role_model = config.models.roles.find(role);
      if (role_model == config.models.roles.end()) {
        return invalid("models.roles",
                       std::string("role '") + role + "' is missing");
      }
      const auto id = role_model->second.ids.find(config.models.default_provider);
      if (id == role_model->second.ids.end()) {
        return invalid("models.roles." + std::string(role),
                       "no model id for provider '" +
                           config.models.default_provider + "'");
      }
      options.role_models[role] = id->second;
    }
    const auto planner = config.models.roles.find("planner");
    if (planner != config.models.roles.end()) {
      const auto id = planner->second.ids.find(config.models.default_provider);
      if (id == planner->second.ids.end()) {
        return invalid("models.roles.planner",
                       "no model id for provider '" +
                           config.models.default_provider + "'");
      }
      options.design_model = id->second;
    }
    for (const auto& [role, role_model] : config.models.roles) {
      nlohmann::json role_options = nlohmann::json::object();
      if (role_model.temperature.has_value()) {
        role_options["temperature"] = *role_model.temperature;
      }
      if (role_model.max_output_tokens.has_value()) {
        role_options["max_output_tokens"] = *role_model.max_output_tokens;
      }
      if (!role_options.empty()) {
        options.role_options[role] = std::move(role_options);
      }
    }
  }

  if (config.budget.max_usd_total.has_value()) {
    options.budget_usd = *config.budget.max_usd_total;
  }
  if (config.budget.max_tokens_total.has_value()) {
    options.budget_tokens = *config.budget.max_tokens_total;
  }
  if (config.budget.max_iterations_per_milestone.has_value()) {
    options.max_iterations_per_milestone =
        *config.budget.max_iterations_per_milestone;
  }
  if (config.budget.max_attempts_per_defect.has_value()) {
    options.max_attempts_per_defect = *config.budget.max_attempts_per_defect;
  }
  if (config.review.diff_threshold_lines.has_value()) {
    options.review_diff_threshold_lines = *config.review.diff_threshold_lines;
  }
  if (config.sandbox.cpu.has_value()) {
    options.sandbox_cpus = *config.sandbox.cpu;
  }
  if (config.sandbox.memory_gb.has_value()) {
    options.sandbox_memory_gb = *config.sandbox.memory_gb;
  }
  if (config.sandbox.job_timeout_min.has_value()) {
    options.gate_timeout_min = *config.sandbox.job_timeout_min;
  }
  return {};
}

}  // namespace cf::cli
