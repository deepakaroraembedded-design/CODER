#include "cf/orch/waivers.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace cf::orch {
namespace {

bool waiver_scope_covers(std::string_view scope, std::string_view message) {
  if (scope.empty() || scope == "*") {
    return true;
  }
  if (message.find(scope) != std::string_view::npos) {
    return true;
  }
  // "src/bus.cpp:42" also covers a finding that only names the file.
  const std::size_t colon = scope.rfind(':');
  if (colon != std::string_view::npos && colon > 0) {
    const std::string_view file = scope.substr(0, colon);
    if (message.find(file) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

bool finding_is_waived(const gates::GateFinding& finding,
                       const std::vector<store::WaiverRecord>& waivers) {
  return std::any_of(waivers.begin(), waivers.end(),
                     [&finding](const store::WaiverRecord& waiver) {
                       const bool rule_matches =
                           waiver.rule == finding.gate ||
                           finding.message.find(waiver.rule) != std::string::npos;
                       return rule_matches &&
                              waiver_scope_covers(waiver.scope, finding.message);
                     });
}

}  // namespace

void apply_waivers(gates::GateReport& report,
                   const std::vector<store::WaiverRecord>& waivers) {
  for (auto& result : report.results) {
    result.waived = false;
    if (result.passed || result.skipped) {
      continue;
    }
    if (result.findings.empty()) {
      // A gate can fail without structured findings (for example a failed
      // launch). Only a rule-wide waiver can cover it.
      result.waived =
          std::any_of(waivers.begin(), waivers.end(),
                      [&result](const store::WaiverRecord& waiver) {
                        return waiver.rule == result.gate &&
                               (waiver.scope.empty() || waiver.scope == "*");
                      });
      continue;
    }
    result.waived = std::all_of(
        result.findings.begin(), result.findings.end(),
        [&waivers](const gates::GateFinding& finding) {
          return finding_is_waived(finding, waivers);
        });
  }
  report.passed = std::all_of(
      report.results.begin(), report.results.end(),
      [](const gates::GateResult& result) {
        return !result.skipped && (result.passed || result.waived);
      });
}

}  // namespace cf::orch
