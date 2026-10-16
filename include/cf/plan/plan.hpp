// cf-plan: implementation plan parser and validator (design sections 6.2,
// 8.1; FR-01, FR-18).
//
// Accepts a plan as pure YAML or as Markdown with YAML front matter, validates
// it against the plan schema, checks the milestone dependency graph, and (when
// a profile lookup is supplied) rejects targets with no toolchain profile.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::plan {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kParseFailed = "PLAN_PARSE_FAILED";
inline constexpr const char* kSchemaInvalid = "PLAN_SCHEMA_INVALID";
inline constexpr const char* kDependencyInvalid = "PLAN_DEPENDENCY_INVALID";
inline constexpr const char* kNoToolchainProfile = "PLAN_NO_TOOLCHAIN_PROFILE";
}  // namespace error_codes

struct MilestoneSpec {
  std::string id;
  std::string title;
  std::vector<std::string> requirements;  // requirement IDs, e.g. REQ-010
  std::vector<std::string> depends_on;    // milestone IDs
  std::vector<std::string> acceptance;    // acceptance criteria
  // Refined-plan fields (design section 6.13.2); optional in the input plan.
  std::vector<std::string> writes;             // components and paths it writes
  std::vector<std::string> uses;               // public interfaces it uses
  std::vector<std::string> writes_interfaces;  // public interfaces it changes
};

struct TargetSpec {
  std::string language;        // cpp, c, python, typescript, java, go
  std::string standard;        // c++20, c11, 3.12, ...
  std::string build_system;    // cmake, meson, make, pip, npm, maven, gradle, go
  std::string test_framework;  // gtest, catch2, unity, cmocka, pytest, ...
  std::vector<std::string> platforms;
};

struct BudgetSpec {
  std::optional<double> max_usd_total;
  std::optional<uint64_t> max_tokens_total;
  std::optional<uint32_t> max_iterations_per_milestone;
  std::optional<uint32_t> max_attempts_per_defect;
  std::optional<double> max_wall_clock_hours;
};

struct Plan {
  std::string plan_id;
  std::string product;
  std::string version;
  std::string requirements_ref;
  std::vector<MilestoneSpec> milestones;
  TargetSpec target;
  std::vector<std::string> forbidden;  // constraints.forbidden
  std::map<std::string, std::string, std::less<>> role_models;  // planner|coder|reviewer|qa
  std::string inference = "fireworks";  // or "local-vllm" (design section 13.1)
  BudgetSpec budget;
  std::string source_path;

  const MilestoneSpec* find_milestone(std::string_view id) const;

  // Milestone IDs in dependency order (Kahn, stable by input order). Empty
  // only if the plan was built without validation and contains a cycle.
  std::vector<std::string> topological_order() const;
};

struct ValidationOptions {
  // FR-18: returns true when a toolchain profile exists for the target. When
  // unset, the check is skipped (the profile registry is wired in by the
  // caller so cf-plan does not depend on cf-gates).
  std::function<bool(const TargetSpec&)> profile_exists;
};

Result<Plan> parse_plan_string(std::string_view text,
                               const ValidationOptions& options = {});
Result<Plan> parse_plan_file(const std::filesystem::path& path,
                             const ValidationOptions& options = {});

}  // namespace cf::plan
