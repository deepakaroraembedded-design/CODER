// cf-orch: escalation bundle (design section 12).
//
// When a run escalates, CF writes everything a person needs into a bundle:
// what was being attempted, the milestone states, the operation history, the
// recent transitions and the cost so far. The model-written root-cause
// summary and the last diffs are added by the caller when available.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "cf/common/result.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

namespace error_codes {
inline constexpr const char* kBundleWriteFailed = "ORCH_BUNDLE_WRITE_FAILED";
}  // namespace error_codes

// Writes `<dir>/esc-<now>/bundle.json` and returns the file path.
Result<std::string> write_escalation_bundle(store::RunStore& store,
                                            std::string_view run_id,
                                            const std::filesystem::path& dir,
                                            std::string_view reason,
                                            store::UnixTime now);

}  // namespace cf::orch
