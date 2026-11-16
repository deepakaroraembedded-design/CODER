// cf-orch: the final run report (design sections 11.2, 14).
//
// Builds report.json and report.md from the run store, agent usage, metrics
// and escalation bundles. The CLI (`cf report`) and the run process (before
// a run returns Done) share this writer.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "cf/common/result.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct RunReport {
  std::filesystem::path json_path;
  std::filesystem::path markdown_path;
};

// Writes report.json/report.md into out_dir (created if needed). A missing
// plan only drops the requirement traceability rows.
Result<RunReport> write_run_report(store::RunStore& store,
                                   std::string_view run_id,
                                   const plan::Plan* plan,
                                   const std::filesystem::path& run_dir,
                                   const std::filesystem::path& out_dir);

}  // namespace cf::orch
