// cf-orch: the run summary shared by the CLI report and the GitHub PR body
// (design sections 10.4.8, 14).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct RunSummary {
  store::RunRecord run;
  nlohmann::json milestones = nlohmann::json::array();
  nlohmann::json requirements = nlohmann::json::array();
  nlohmann::json defects = nlohmann::json::array();
  nlohmann::json waivers = nlohmann::json::array();
  nlohmann::json waiver_requests = nlohmann::json::array();
  // Agent-turn usage from runs/<run>/usage-<role>.jsonl (opencode reports
  // cost and tokens per session; design sections 6.11, 14).
  nlohmann::json agent_usage_by_role = nlohmann::json::array();
  nlohmann::json agent_usage_by_milestone = nlohmann::json::array();
  double usd = 0.0;    // store ledger plus agent turns
  int64_t tokens = 0;  // prompt + completion, both sources
};

// Reads the run, milestone/gate history, defects, requirement traceability
// and cost from the store, and the agent-turn usage files in `run_dir` when
// it is given. `plan` may be null when the stored plan is unreadable;
// traceability is then empty.
Result<RunSummary> build_run_summary(store::RunStore& store,
                                     std::string_view run_id,
                                     const plan::Plan* plan,
                                     const std::filesystem::path& run_dir = {});

// Markdown for the milestone table, requirement traceability matrix,
// defects, waivers and agent usage. The caller adds the run header, cost and
// any escalations.
std::string render_run_summary_markdown(const RunSummary& summary);

}  // namespace cf::orch
