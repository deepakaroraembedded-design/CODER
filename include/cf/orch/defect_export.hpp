// cf-orch: defect export to GitHub Issues (design section 6.10).
//
// Every defect in the run store is mirrored as a GitHub issue: one issue per
// defect, labelled `cf-defect`, `severity:<level>` and `milestone:<id>`, with
// the hidden fingerprint marker so a recurring defect updates the existing
// issue instead of opening a new one. Status changes sync (close on VERIFIED,
// reopen on REOPENED), and a human closing the issue as "not planned" is
// recorded as WONT_FIX. Bodies are secret-scanned; restricted runs post only
// IDs and a link to the internal report.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cf/github/client.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct DefectExportConfig {
  std::string owner;
  std::string repo;
  // Restricted runs post only IDs and the report link (design section 6.10).
  bool restricted = false;
  // Base for report links, e.g. "artifact://runs/R12".
  std::string report_base;
  // When set, a human closing the issue as "not planned" is pulled back into
  // the store as WONT_FIX (the only route to that status).
  github::Client* client = nullptr;
};

class DefectExporter {
 public:
  DefectExporter(store::RunStore& store, Dispatcher& dispatcher,
                 DefectExportConfig config);

  // Records (idempotently) and dispatches the issue operations for the run's
  // defects, then pulls human closures back into the store (WONT_FIX).
  Result<std::size_t> sync(std::string_view run_id, uint64_t epoch,
                           store::UnixTime now);

  // The issue body for a defect (exposed for tests).
  std::string build_body(std::string_view run_id, const store::DefectRecord& defect,
                         bool redact) const;

 private:
  Result<int64_t> confirmed_issue_number(std::string_view run_id,
                                         std::string_view defect_id) const;

  Status ensure_issue(std::string_view run_id, const store::DefectRecord& defect,
                      int64_t number, store::UnixTime now);

  Status sync_status(std::string_view run_id, const store::DefectRecord& defect,
                     int64_t number, store::UnixTime now);

  store::RunStore& store_;
  Dispatcher& dispatcher_;
  DefectExportConfig config_;
};

}  // namespace cf::orch
