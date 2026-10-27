// cf-orch: the run summary shared by the CLI report and the GitHub PR body
// (design sections 10.4.8, 14).
#pragma once

#include <cstdint>
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
  double usd = 0.0;
  int64_t tokens = 0;
};

// Reads the run, milestone/gate history, defects, requirement traceability
// and cost from the store. `plan` may be null when the stored plan is
// unreadable; traceability is then empty.
Result<RunSummary> build_run_summary(store::RunStore& store,
                                     std::string_view run_id,
                                     const plan::Plan* plan);

// Markdown for the milestone table, requirement traceability matrix and
// defect list. The caller adds the run header, cost and any escalations.
std::string render_run_summary_markdown(const RunSummary& summary);

}  // namespace cf::orch
