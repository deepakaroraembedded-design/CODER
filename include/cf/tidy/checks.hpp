// cf-tidy: the company guardrail checks (design section 6.5/6.7.1).
//
// The checks are exposed as a small library so the gate driver and fixture
// tests share one implementation. `cf-tidy` is the gate-facing executable;
// it deliberately does not depend on clang-tidy's runtime (the pinned LLVM
// package is static, so clang-tidy plugin loading is not reliable).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::tidy {

struct TidyOptions {
  // Regular expression matched against the fully-qualified namespace name.
  // Empty means no namespace is exempt.
  std::string allowed_namespaces;
};

struct TidyDiagnostic {
  std::string check;  // e.g. "cf-swt-no-raw-new"
  std::string message;
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
};

struct CheckInfo {
  std::string id;           // cf-swt-*
  std::string rule;         // company rule / guideline id
  std::string description;
};

// The checks compiled into this driver, for --list-checks and the rule
// coverage catalog.
std::vector<CheckInfo> implemented_checks();

// Runs every company check over `files` using the compilation database in
// `build_dir` (a compile_commands.json directory). Returns findings sorted by
// file and line; parse/compilation failures are returned as errors.
Result<std::vector<TidyDiagnostic>> run_company_checks(
    const std::filesystem::path& build_dir,
    const std::vector<std::filesystem::path>& files,
    const TidyOptions& options = {});

}  // namespace cf::tidy
