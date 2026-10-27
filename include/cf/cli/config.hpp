// cf-cli: the cf.yaml configuration file (design section 15).
//
// `cf run` loads it with --config, or ./cf.yaml when that exists. Precedence
// is command-line flags > cf.yaml > built-in defaults. The parsed schema is
// strict: an unknown key fails with CLI_CONFIG_INVALID naming the key, so a
// configuration never silently half-applies; sections whose knobs have no
// consumer yet (rag, reference_docs, gate coverage/mutation, github policies,
// waiver deadlines) are rejected the same way until they are implemented.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cf/cli/commands.hpp"
#include "cf/common/result.hpp"

namespace cf::cli {

namespace config_error_codes {
inline constexpr const char* kOpenFailed = "CLI_CONFIG_OPEN_FAILED";
inline constexpr const char* kInvalid = "CLI_CONFIG_INVALID";
}  // namespace config_error_codes

struct FrameworkConfig {
  struct Run {
    std::optional<std::string> workspace;
    std::optional<std::string> repo;
    std::optional<std::string> base_branch;
  };
  Run run;

  struct Github {
    std::optional<std::string> app_id;
    std::optional<std::string> installation_id;
    // A filesystem path to the PEM. secretmanager:// references are rejected
    // at load time (not resolvable in this build).
    std::optional<std::string> private_key_ref;
    // Pre-push policy (design sections 10.4.6, 15).
    std::optional<std::string> branch_prefix;
    std::optional<std::vector<std::string>> protected_paths;
    std::optional<std::uint64_t> max_file_mb;
    // Pull-request policy (design sections 10.4.4, 10.4.8, 15).
    struct Pr {
      std::optional<bool> draft_until_done;
      std::optional<std::vector<std::string>> labels;
      std::optional<bool> request_codeowners;
    };
    Pr pr;
  };
  Github github;

  struct Provider {
    std::string base_url;
    std::string api_key_env;
  };
  // Keyed by provider name ("fireworks", "local-vllm", ...).
  std::map<std::string, Provider, std::less<>> providers;

  struct RoleModel {
    std::string family;  // informational (design decisions D3, D11)
    // Per-provider model id: the key is the provider name (design section
    // 15: fireworks / local-vllm), the value the provider's model id.
    std::map<std::string, std::string, std::less<>> ids;
    std::optional<double> temperature;
    std::optional<std::uint32_t> max_output_tokens;
  };
  struct Models {
    std::string default_provider = "fireworks";
    // coder, reviewer, qa and planner (design sections 6.2, 6.6).
    std::map<std::string, RoleModel, std::less<>> roles;
  };
  Models models;

  // Defaults only: a plan's budget block overrides them (design Q5). Every
  // run must have some limit so it always ends (goal G5).
  struct Budget {
    std::optional<double> max_usd_total;
    std::optional<std::uint64_t> max_tokens_total;
    std::optional<std::uint32_t> max_iterations_per_milestone;
    std::optional<std::uint32_t> max_attempts_per_defect;
    // Active time only: states that wait for people do not count (design
    // sections 12, 15).
    std::optional<double> max_wall_clock_hours;
  };
  Budget budget;

  struct Approvals {
    // Business days (design section 12.1); the plan overrides these.
    std::optional<std::uint32_t> design_approval_business_days;
    std::optional<std::uint32_t> escalation_business_days;
  };
  Approvals approvals;

  struct Gates {
    std::optional<std::string> profiles_dir;
  };
  Gates gates;

  struct Review {
    std::optional<std::uint32_t> diff_threshold_lines;
  };
  Review review;

  struct Sandbox {
    std::optional<double> cpu;
    std::optional<double> memory_gb;
    std::optional<std::uint32_t> job_timeout_min;
  };
  Sandbox sandbox;
};

// Parses and validates cf.yaml. A missing or unreadable file is
// CLI_CONFIG_OPEN_FAILED; schema violations are CLI_CONFIG_INVALID and name
// the offending key.
Result<FrameworkConfig> load_config(const std::filesystem::path& path);

// Applies the configuration to the run options. Callers apply command-line
// flags afterwards (only the flags that were passed), giving flags > cf.yaml
// > built-in defaults precedence. A plan's own budget block still wins over
// budget defaults (design section 15, Q5).
//
// The run never mixes providers (design section 13.1): when the config names
// role models, every agent role must resolve a model id for the default
// provider, and the resolved set replaces the built-in pins.
Status apply_config(ExecuteOptions& options, const FrameworkConfig& config);

}  // namespace cf::cli
