// cf-orch: the run metrics snapshot (design section 14).
//
// The values use the metric names and label sets from the design, computed
// from the run store and the agent-turn usage files. The structured snapshot
// is embedded in the final report; an OpenTelemetry exporter is not built
// yet.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct MetricSample {
  std::string name;
  nlohmann::json labels = nlohmann::json::object();
  double value = 0.0;
};

// Collects cf_gate_pass_ratio, cf_iterations, cf_defects_open,
// cf_llm_tokens_total, cf_llm_usd_total and cf_escalations_total. `run_dir`
// is the directory holding usage-<role>.jsonl; empty skips agent usage.
Result<std::vector<MetricSample>> collect_run_metrics(
    store::RunStore& store, std::string_view run_id,
    const std::filesystem::path& run_dir = {});

}  // namespace cf::orch
