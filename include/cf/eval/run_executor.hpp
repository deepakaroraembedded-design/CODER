// cf-eval: the real plan executor (design sections 17.2, 18.2 Phase 0b).
//
// Drives one golden plan through the production pipeline: a fresh scratch
// repository, a run store, the run orchestrator with real opencode servers
// and the pinned models, then the plan's hidden acceptance tests against the
// integration worktree. Every metric in the returned PlanOutcome is read back
// from the run store and the run directory, never inferred.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/eval/benchmark.hpp"
#include "cf/eval/score.hpp"
#include "cf/eval/suite.hpp"

namespace cf::eval {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kRunExecutorFailed = "EVAL_RUN_EXECUTOR_FAILED";
inline constexpr const char* kRunExecutorConfig = "EVAL_RUN_EXECUTOR_CONFIG";
}  // namespace error_codes

struct RunExecutorOptions {
  std::filesystem::path workspace_root;  // scratch root for every plan run
  std::filesystem::path suite_dir;       // manifest base directory (plan files)
  std::filesystem::path profiles_dir{"profiles"};
  std::filesystem::path hidden_dir;  // default: suite_dir/hidden
  std::string opencode_binary = "opencode";
  std::string mcp_binary;
  std::string docs_dir;
  std::string sandbox_image;
  std::string provider_base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key_env = "FIREWORKS_API_KEY";
  std::string base_branch = "main";
  std::map<std::string, std::string, std::less<>> role_models;
  // Extra opencode model options per role (see RoleConfigSpec).
  std::map<std::string, nlohmann::json, std::less<>> role_options;
  std::map<std::string, ModelPrice, std::less<>> prices;
  std::vector<std::pair<std::string, std::string>> environment;
  std::chrono::milliseconds turn_timeout{1800000};
  std::chrono::milliseconds step_timeout{1800000};
  std::chrono::milliseconds hidden_timeout{900000};
  bool keep_scratch = true;
};

class RunProcessExecutor final : public PlanExecutor {
 public:
  RunProcessExecutor(RunExecutorOptions options, process::ProcessExecutor& executor);

  Result<PlanOutcome> run(const PlanEntry& plan, int repeat) override;

  // <workspace_root>/<plan_id>/repeat-<n>
  std::filesystem::path scratch_directory(const std::string& plan_id, int repeat) const;

  static std::string run_id_for(const std::string& plan_id, int repeat);

 private:
  RunExecutorOptions options_;
  process::ProcessExecutor& executor_;
};

// Sums the agent-turn usage files (`usage-<role>.jsonl`) written by the run
// process: provider cost when present, otherwise an estimate from the
// benchmark's price table. Returns the total in USD.
double sum_agent_usage_usd(
    const std::filesystem::path& run_dir,
    const std::map<std::string, std::string, std::less<>>& role_models,
    const std::map<std::string, ModelPrice, std::less<>>& prices);

}  // namespace cf::eval
