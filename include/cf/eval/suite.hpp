// cf-eval: the golden evaluation suite harness (design section 17.2).
//
// Loads a stratified suite manifest — functional plans per language and tier,
// reliability, security, governance and conflict scenarios — records plan run
// outcomes, and scores them against the G1, security and recovery targets with
// confidence intervals and a held-out split.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::eval {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kSuiteParseFailed = "EVAL_SUITE_PARSE_FAILED";
inline constexpr const char* kSuiteInvalid = "EVAL_SUITE_INVALID";
inline constexpr const char* kOutcomeInvalid = "EVAL_OUTCOME_INVALID";
}  // namespace error_codes

enum class PlanCategory { Functional, Reliability, Security, Governance, Conflict };

std::string to_string(PlanCategory category);
std::optional<PlanCategory> category_from_string(std::string_view text);

struct PlanEntry {
  std::string id;
  PlanCategory category = PlanCategory::Functional;
  std::string language;  // functional plans only
  std::string tier;      // T1..T4, functional plans only
  std::filesystem::path plan_path;  // relative to the manifest directory
  bool held_out = false;
};

// G1 applies to functional plans; the security and recovery targets apply to
// their scenario categories (design sections 17.2, 17.3).
struct SuiteTargets {
  double completion = 0.80;  // milestones reaching the Definition of Done
  double security = 1.00;
  double recovery = 1.00;
};

struct SuiteManifest {
  std::string suite_id;
  int repeats = 3;
  SuiteTargets targets;
  std::vector<PlanEntry> plans;
  std::filesystem::path base_dir;  // directory the plan paths are relative to
};

Result<SuiteManifest> load_suite_manifest(const std::filesystem::path& path);
Result<SuiteManifest> parse_suite_manifest(std::string_view yaml,
                                           const std::filesystem::path& base_dir = {});

// Checks every plan file exists and parses as an implementation plan (cf-plan);
// returns the plan ids in manifest order.
Result<std::vector<std::string>> validate_suite_plans(const SuiteManifest& manifest);

}  // namespace cf::eval
