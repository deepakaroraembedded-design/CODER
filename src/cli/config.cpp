#include "cf/cli/config.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "cf/common/duration.hpp"
#include "cf/rag/types.hpp"

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
const std::vector<std::string_view> kUnsupportedTopLevel = {};

const std::vector<std::string_view> kTopLevel = {
    "run", "github", "providers", "models", "approvals", "budget", "rag",
    "reference_docs", "gates", "review", "sandbox",
};

Status read_business_days(const YAML::Node& node, std::string_view key,
                          std::optional<std::uint32_t>& out) {
  if (!node) {
    return {};
  }
  std::string text;
  try {
    text = node.as<std::string>();
  } catch (const YAML::Exception& e) {
    return invalid(key, e.what());
  }
  const auto parsed = cf::parse_business_day_duration(text);
  if (!parsed) {
    return invalid(key, "expected a positive business-day duration like '5d'");
  }
  out = *parsed;
  return {};
}

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
      if (key == "app_id" || key == "installation_id" ||
          key == "private_key_ref" || key == "signing_key_ref" ||
          key == "branch_prefix" || key == "protected_paths" ||
          key == "max_file_mb" || key == "push_on" ||
          key == "branch_retention_days" || key == "pr") {
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
    if (auto status = read_optional(github["signing_key_ref"],
                                    "github.signing_key_ref",
                                    config.github.signing_key_ref);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.github.signing_key_ref.has_value() &&
        config.github.signing_key_ref->find("secretmanager://") == 0) {
      return invalid("github.signing_key_ref",
                     "secret manager references are not resolvable in this "
                     "build; use an SSH private key file path");
    }
    if (auto status = read_optional(github["branch_prefix"],
                                    "github.branch_prefix",
                                    config.github.branch_prefix);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(github["protected_paths"],
                                    "github.protected_paths",
                                    config.github.protected_paths);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(github["max_file_mb"], "github.max_file_mb",
                                    config.github.max_file_mb);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(github["branch_retention_days"],
                                    "github.branch_retention_days",
                                    config.github.branch_retention_days);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (github["push_on"]) {
      std::vector<std::string> push_on;
      if (auto status =
              read_into(github["push_on"], "github.push_on", push_on);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (push_on.empty()) {
        return invalid("github.push_on",
                       "must name at least one of milestone_done, run_done, "
                       "escalation");
      }
      std::vector<std::string> deduped;
      for (const auto& event : push_on) {
        if (event != "milestone_done" && event != "run_done" &&
            event != "escalation") {
          return invalid("github.push_on",
                         "unknown push event '" + event +
                             "' (expected milestone_done, run_done or "
                             "escalation)");
        }
        if (std::find(deduped.begin(), deduped.end(), event) == deduped.end()) {
          deduped.push_back(event);
        }
      }
      config.github.push_on = std::move(deduped);
    }
    if (const YAML::Node pr = github["pr"]) {
      if (!pr.IsMap()) {
        return invalid("github.pr", "must be a mapping");
      }
      auto checked = check_keys(pr, "github.pr",
                                {"draft_until_done", "labels",
                                 "request_codeowners"});
      if (!checked) {
        return tl::make_unexpected(checked.error());
      }
      if (auto status =
              read_optional(pr["draft_until_done"],
                            "github.pr.draft_until_done",
                            config.github.pr.draft_until_done);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (auto status = read_optional(pr["labels"], "github.pr.labels",
                                      config.github.pr.labels);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (auto status =
              read_optional(pr["request_codeowners"],
                            "github.pr.request_codeowners",
                            config.github.pr.request_codeowners);
          !status) {
        return tl::make_unexpected(status.error());
      }
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
    auto checked = check_keys(models, "models",
                              {"default_provider", "roles", "embeddings"});
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
    if (const YAML::Node embeddings = models["embeddings"]) {
      if (!embeddings.IsMap()) {
        return invalid("models.embeddings",
                       "must be a mapping of provider to embedding model");
      }
      for (const auto& entry : embeddings) {
        const std::string provider = entry.first.as<std::string>();
        const YAML::Node node = entry.second;
        if (!node.IsMap()) {
          return invalid("models.embeddings." + provider, "must be a mapping");
        }
        auto keys = check_keys(node, "models.embeddings." + provider,
                               {"model", "dim", "index"});
        if (!keys) {
          return tl::make_unexpected(keys.error());
        }
        FrameworkConfig::EmbeddingModel embedding;
        if (auto status = read_into(node["model"],
                                    "models.embeddings." + provider + ".model",
                                    embedding.model);
            !status) {
          return tl::make_unexpected(status.error());
        }
        if (embedding.model.empty()) {
          return invalid("models.embeddings." + provider + ".model",
                         "is required");
        }
        if (auto status = read_optional(node["dim"],
                                        "models.embeddings." + provider + ".dim",
                                        embedding.dim);
            !status) {
          return tl::make_unexpected(status.error());
        }
        if (embedding.dim.has_value() && *embedding.dim == 0) {
          return invalid("models.embeddings." + provider + ".dim",
                         "must be positive");
        }
        if (auto status = read_optional(node["index"],
                                        "models.embeddings." + provider + ".index",
                                        embedding.index);
            !status) {
          return tl::make_unexpected(status.error());
        }
        if (embedding.index.has_value() && *embedding.index != "hosted" &&
            *embedding.index != "local") {
          return invalid("models.embeddings." + provider + ".index",
                         "must be 'hosted' or 'local'");
        }
        config.models.embeddings[provider] = std::move(embedding);
      }
    }
  }

  if (const YAML::Node approvals = root["approvals"]) {
    if (!approvals.IsMap()) {
      return invalid("approvals", "must be a mapping of waiting state to deadline");
    }
    for (const auto& entry : approvals) {
      const std::string key = entry.first.as<std::string>();
      if (key == "design_approval" || key == "escalation" || key == "waiver") {
        continue;
      }
      return unknown_key(key, "approvals");
    }
    if (auto status = read_business_days(approvals["design_approval"],
                                         "approvals.design_approval",
                                         config.approvals.design_approval_business_days);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_business_days(approvals["escalation"],
                                         "approvals.escalation",
                                         config.approvals.escalation_business_days);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_business_days(approvals["waiver"], "approvals.waiver",
                                         config.approvals.waiver_business_days);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  if (const YAML::Node reference = root["reference_docs"]) {
    if (!reference.IsMap()) {
      return invalid("reference_docs", "must be a mapping");
    }
    auto checked = check_keys(reference, "reference_docs",
                              {"manifest", "index_version", "version_source",
                               "on_missing_version"});
    if (!checked) {
      return tl::make_unexpected(checked.error());
    }
    if (auto status = read_optional(reference["manifest"], "reference_docs.manifest",
                                    config.reference_docs.manifest);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (!config.reference_docs.manifest.has_value()) {
      return invalid("reference_docs.manifest",
                     "is required when reference_docs is configured");
    }
    if (auto status = read_optional(reference["index_version"],
                                    "reference_docs.index_version",
                                    config.reference_docs.index_version);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.reference_docs.index_version.has_value() &&
        *config.reference_docs.index_version != "pinned_at_run_start") {
      return invalid("reference_docs.index_version",
                     "only 'pinned_at_run_start' is supported");
    }
    if (auto status = read_optional(reference["version_source"],
                                    "reference_docs.version_source",
                                    config.reference_docs.version_source);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.reference_docs.version_source.has_value() &&
        *config.reference_docs.version_source != "auto" &&
        *config.reference_docs.version_source != "none") {
      return invalid("reference_docs.version_source",
                     "must be 'auto' or 'none'");
    }
    if (auto status = read_optional(reference["on_missing_version"],
                                    "reference_docs.on_missing_version",
                                    config.reference_docs.on_missing_version);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.reference_docs.on_missing_version.has_value() &&
        *config.reference_docs.on_missing_version != "report" &&
        *config.reference_docs.on_missing_version != "fail") {
      return invalid("reference_docs.on_missing_version",
                     "must be 'report' or 'fail'");
    }
  }

  if (const YAML::Node rag = root["rag"]) {
    if (!rag.IsMap()) {
      return invalid("rag", "must be a mapping");
    }
    auto checked = check_keys(rag, "rag",
                              {"index", "top_k", "hybrid", "precedence"});
    if (!checked) {
      return tl::make_unexpected(checked.error());
    }
    if (auto status = read_optional(rag["index"], "rag.index", config.rag.index);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.rag.index.has_value() && *config.rag.index != "hnswlib" &&
        *config.rag.index != "brute-force") {
      return invalid("rag.index",
                     "expected 'hnswlib' or 'brute-force'");
    }
    if (auto status = read_optional(rag["top_k"], "rag.top_k",
                                    config.rag.top_k);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.rag.top_k.has_value() &&
        (*config.rag.top_k == 0 || *config.rag.top_k > 1000)) {
      return invalid("rag.top_k", "must be between 1 and 1000");
    }
    if (const YAML::Node hybrid = rag["hybrid"]) {
      if (!hybrid.IsMap()) {
        return invalid("rag.hybrid", "must be a mapping");
      }
      auto hybrid_checked =
          check_keys(hybrid, "rag.hybrid", {"bm25_weight", "vector_weight"});
      if (!hybrid_checked) {
        return tl::make_unexpected(hybrid_checked.error());
      }
      if (auto status =
              read_optional(hybrid["bm25_weight"], "rag.hybrid.bm25_weight",
                            config.rag.bm25_weight);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (auto status =
              read_optional(hybrid["vector_weight"], "rag.hybrid.vector_weight",
                            config.rag.vector_weight);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if ((config.rag.bm25_weight.has_value() &&
           *config.rag.bm25_weight < 0.0) ||
          (config.rag.vector_weight.has_value() &&
           *config.rag.vector_weight < 0.0)) {
        return invalid("rag.hybrid", "weights must not be negative");
      }
      if (config.rag.bm25_weight.has_value() &&
          config.rag.vector_weight.has_value() &&
          *config.rag.bm25_weight == 0.0 && *config.rag.vector_weight == 0.0) {
        return invalid("rag.hybrid", "at least one weight must be positive");
      }
    }
    if (auto status =
            read_optional(rag["precedence"], "rag.precedence",
                          config.rag.precedence);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.rag.precedence.has_value()) {
      std::set<rag::DocType> seen;
      for (const auto& name : *config.rag.precedence) {
        const auto type = rag::doc_type_from_string(name);
        if (!type) {
          return invalid("rag.precedence",
                         "unknown document type '" + name + "'");
        }
        if (!seen.insert(*type).second) {
          return invalid("rag.precedence",
                         "duplicate document type '" + name + "'");
        }
      }
      for (const rag::DocType type :
           {rag::DocType::Policy, rag::DocType::Requirements,
            rag::DocType::Architecture, rag::DocType::Design,
            rag::DocType::Guideline, rag::DocType::Pattern,
            rag::DocType::ExternalReference}) {
        if (seen.count(type) == 0) {
          config.rag.precedence->push_back(std::string(rag::to_string(type)));
        }
      }
    }
  }

  if (const YAML::Node budget = root["budget"]) {
    for (const auto& entry : budget) {
      const std::string key = entry.first.as<std::string>();
      if (key == "max_usd_total" || key == "max_tokens_total" ||
          key == "max_iterations_per_milestone" ||
          key == "max_attempts_per_defect" ||
          key == "max_wall_clock_hours") {
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
    if (auto status = read_optional(budget["max_wall_clock_hours"],
                                    "budget.max_wall_clock_hours",
                                    config.budget.max_wall_clock_hours);
        !status) {
      return tl::make_unexpected(status.error());
    }
  }

  if (const YAML::Node gates = root["gates"]) {
    for (const auto& entry : gates) {
      const std::string key = entry.first.as<std::string>();
      if (key == "profiles_dir" || key == "profile" || key == "coverage" ||
          key == "mutation") {
        continue;
      }
      return unsupported("gates." + key);
    }
    if (auto status = read_optional(gates["profiles_dir"], "gates.profiles_dir",
                                    config.gates.profiles_dir);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (auto status = read_optional(gates["profile"], "gates.profile",
                                    config.gates.profile);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.gates.profile.has_value() && config.gates.profile->empty()) {
      return invalid("gates.profile", "must not be empty; use 'auto'");
    }
    if (const YAML::Node coverage = gates["coverage"]) {
      if (!coverage.IsMap()) {
        return invalid("gates.coverage", "must be a mapping");
      }
      auto checked = check_keys(coverage, "gates.coverage",
                                {"line", "branch", "scope"});
      if (!checked) {
        return tl::make_unexpected(checked.error());
      }
      if (auto status = read_optional(coverage["line"], "gates.coverage.line",
                                      config.gates.coverage.line);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (auto status = read_optional(coverage["branch"], "gates.coverage.branch",
                                      config.gates.coverage.branch);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (auto status = read_optional(coverage["scope"], "gates.coverage.scope",
                                      config.gates.coverage.scope);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (config.gates.coverage.line.has_value() &&
          (*config.gates.coverage.line < 0.0 ||
           *config.gates.coverage.line > 1.0)) {
        return invalid("gates.coverage.line", "must be between 0 and 1");
      }
      if (config.gates.coverage.branch.has_value() &&
          (*config.gates.coverage.branch < 0.0 ||
           *config.gates.coverage.branch > 1.0)) {
        return invalid("gates.coverage.branch", "must be between 0 and 1");
      }
      if (config.gates.coverage.scope.has_value() &&
          *config.gates.coverage.scope != "changed" &&
          *config.gates.coverage.scope != "all") {
        return invalid("gates.coverage.scope", "must be 'changed' or 'all'");
      }
    }
    if (const YAML::Node mutation = gates["mutation"]) {
      if (!mutation.IsMap()) {
        return invalid("gates.mutation", "must be a mapping");
      }
      auto checked = check_keys(mutation, "gates.mutation",
                                {"enabled", "min_score"});
      if (!checked) {
        return tl::make_unexpected(checked.error());
      }
      if (auto status = read_optional(mutation["enabled"], "gates.mutation.enabled",
                                      config.gates.mutation.enabled);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (auto status = read_optional(mutation["min_score"],
                                      "gates.mutation.min_score",
                                      config.gates.mutation.min_score);
          !status) {
        return tl::make_unexpected(status.error());
      }
      if (config.gates.mutation.min_score.has_value() &&
          (*config.gates.mutation.min_score < 0.0 ||
           *config.gates.mutation.min_score > 1.0)) {
        return invalid("gates.mutation.min_score", "must be between 0 and 1");
      }
    }
  }

  if (const YAML::Node review = root["review"]) {
    for (const auto& entry : review) {
      const std::string key = entry.first.as<std::string>();
      if (key == "diff_threshold_lines" || key == "blocking_severities") {
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
    if (auto status = read_optional(review["blocking_severities"],
                                    "review.blocking_severities",
                                    config.review.blocking_severities);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.review.blocking_severities.has_value()) {
      for (const auto& severity : *config.review.blocking_severities) {
        if (severity != "blocker" && severity != "major" &&
            severity != "minor" && severity != "nit") {
          return invalid("review.blocking_severities",
                         "unknown severity '" + severity + "'");
        }
      }
    }
  }

  if (const YAML::Node sandbox = root["sandbox"]) {
    for (const auto& entry : sandbox) {
      const std::string key = entry.first.as<std::string>();
      if (key == "cpu" || key == "memory_gb" || key == "job_timeout_min" ||
          key == "network") {
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
    if (auto status = read_optional(sandbox["network"], "sandbox.network",
                                    config.sandbox.network);
        !status) {
      return tl::make_unexpected(status.error());
    }
    if (config.sandbox.network.has_value() &&
        *config.sandbox.network != "deny") {
      return invalid("sandbox.network",
                     "gate jobs are network-isolated; only 'deny' is accepted");
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
  if (config.gates.profile.has_value()) {
    options.profile_id = *config.gates.profile;
  }
  if (config.gates.coverage.line.has_value()) {
    options.coverage_line = *config.gates.coverage.line;
  }
  if (config.gates.coverage.branch.has_value()) {
    options.coverage_branch = *config.gates.coverage.branch;
  }
  if (config.gates.mutation.enabled.has_value()) {
    options.mutation_enabled = *config.gates.mutation.enabled;
  }
  if (config.gates.mutation.min_score.has_value()) {
    options.mutation_min_score = *config.gates.mutation.min_score;
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
  if (config.github.signing_key_ref.has_value()) {
    options.github_signing_key_file = *config.github.signing_key_ref;
  }
  if (config.github.branch_prefix.has_value()) {
    options.github_branch_prefix = *config.github.branch_prefix;
  }
  if (config.github.protected_paths.has_value()) {
    options.github_protected_paths = *config.github.protected_paths;
  }
  if (config.github.max_file_mb.has_value()) {
    options.github_max_file_mb = *config.github.max_file_mb;
  }
  if (config.github.push_on.has_value()) {
    options.github_push_on = *config.github.push_on;
  }
  if (config.github.branch_retention_days.has_value()) {
    options.branch_retention_days = *config.github.branch_retention_days;
  }
  if (config.github.pr.draft_until_done.has_value()) {
    options.github_pr_draft_until_done =
        *config.github.pr.draft_until_done;
  }
  if (config.github.pr.labels.has_value()) {
    options.github_pr_labels = *config.github.pr.labels;
  }
  if (config.github.pr.request_codeowners.has_value()) {
    options.github_pr_request_codeowners =
        *config.github.pr.request_codeowners;
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

  // Embeddings (design sections 6.3, 15): use the default provider's entry,
  // a provider starting with its name (local-vllm -> local-vllm-embed), or
  // the only entry.
  if (!config.models.embeddings.empty()) {
    auto embedding = config.models.embeddings.end();
    const auto exact =
        config.models.embeddings.find(config.models.default_provider);
    if (exact != config.models.embeddings.end()) {
      embedding = exact;
    } else {
      const std::string prefix = config.models.default_provider + "-";
      for (auto it = config.models.embeddings.begin();
           it != config.models.embeddings.end(); ++it) {
        if (it->first.rfind(prefix, 0) == 0) {
          embedding = it;
          break;
        }
      }
      if (embedding == config.models.embeddings.end() &&
          config.models.embeddings.size() == 1) {
        embedding = config.models.embeddings.begin();
      }
    }
    if (embedding == config.models.embeddings.end()) {
      return invalid("models.embeddings",
                     "no entry for the default provider '" +
                         config.models.default_provider + "'");
    }
    const auto provider = config.providers.find(embedding->first);
    if (provider == config.providers.end()) {
      return invalid("models.embeddings." + embedding->first,
                     "has no entry in providers");
    }
    options.embeddings_model = embedding->second.model;
    options.embeddings_dim = embedding->second.dim;
    options.embeddings_base_url = provider->second.base_url;
    options.embeddings_api_key_env = provider->second.api_key_env;
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
  if (config.budget.max_wall_clock_hours.has_value()) {
    options.budget_wall_clock_hours = *config.budget.max_wall_clock_hours;
  }
  if (config.approvals.design_approval_business_days.has_value()) {
    options.design_approval_business_days =
        *config.approvals.design_approval_business_days;
  }
  if (config.approvals.escalation_business_days.has_value()) {
    options.escalation_business_days =
        *config.approvals.escalation_business_days;
  }
  if (config.approvals.waiver_business_days.has_value()) {
    options.waiver_business_days = *config.approvals.waiver_business_days;
  }
  if (config.reference_docs.manifest.has_value()) {
    options.reference_docs_manifest = *config.reference_docs.manifest;
  }
  if (config.reference_docs.version_source.has_value()) {
    options.reference_docs_version_source =
        *config.reference_docs.version_source;
  }
  if (config.reference_docs.on_missing_version.has_value()) {
    options.reference_docs_on_missing_version =
        *config.reference_docs.on_missing_version;
  }
  if (config.rag.index.has_value()) {
    options.rag_index = *config.rag.index;
  }
  if (config.rag.top_k.has_value()) {
    options.rag_top_k = *config.rag.top_k;
  }
  if (config.rag.bm25_weight.has_value()) {
    options.rag_bm25_weight = *config.rag.bm25_weight;
  }
  if (config.rag.vector_weight.has_value()) {
    options.rag_vector_weight = *config.rag.vector_weight;
  }
  if (config.rag.precedence.has_value()) {
    options.rag_precedence = *config.rag.precedence;
  }
  if (config.review.diff_threshold_lines.has_value()) {
    options.review_diff_threshold_lines = *config.review.diff_threshold_lines;
  }
  if (config.review.blocking_severities.has_value()) {
    options.review_blocking_severities = *config.review.blocking_severities;
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
