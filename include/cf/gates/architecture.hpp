// cf-gates: config-driven architecture/layering gate (design sections 6.5,
// 6.7 stage 2).
//
// A rule says that files under `from_prefix` may not reference paths under
// `forbidden_prefix` in an include/import. The check is textual and
// language-agnostic; cf-codeintel's AST layering check remains available to
// back ends that have an architecture rules source.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/gates/runner.hpp"

namespace cf::gates {

struct ArchitectureRule {
  std::string id;                // reported with the finding
  std::string from_prefix;       // repo-relative, e.g. "src/ui/"
  std::string forbidden_prefix;  // referenced path prefix, e.g. "src/storage/"
};

// Scans the worktree and returns one finding per violating reference.
Result<std::vector<GateFinding>> check_architecture_rules(
    const std::filesystem::path& worktree,
    const std::vector<ArchitectureRule>& rules);

}  // namespace cf::gates
