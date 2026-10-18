// cf-eval: hidden acceptance tests (design section 17.2).
//
// Each suite plan may carry hidden acceptance tests the agents never see,
// stored with the harness in eval/hidden/<plan_id>/. A spec lists compile
// steps (argv lists, executed with no shell) and test steps. A test step
// counts one test when its exit code is zero; a step that prints a final
// `{"tests": N, "passed": M}` JSON line reports per-case counts instead.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/common/result.hpp"

namespace cf::eval {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kHiddenParseFailed = "EVAL_HIDDEN_PARSE_FAILED";
inline constexpr const char* kHiddenInvalid = "EVAL_HIDDEN_INVALID";
}  // namespace error_codes

struct HiddenStep {
  std::string name;
  std::vector<std::string> argv;
};

struct HiddenTestSpec {
  // Run before the tests; a failure marks every test as failed.
  std::vector<HiddenStep> compile;
  std::vector<HiddenStep> tests;
  // Set when the scenario's pass/fail is the security (or recovery) signal
  // rather than ordinary functional coverage.
  bool security = false;
  bool recovery = false;
  // Crash injection (design section 6.12.5): the runner kills the run
  // process as soon as this milestone is Done (or after this many seconds,
  // whichever comes first) and then resumes. Zero/empty disables it.
  std::string crash_after_milestone;
  int crash_after_seconds = 0;
  std::filesystem::path base_dir;  // directory of the spec file
};

Result<HiddenTestSpec> parse_hidden_test_spec(std::string_view yaml,
                                              const std::filesystem::path& base_dir = {});
Result<HiddenTestSpec> load_hidden_test_spec(const std::filesystem::path& path);

// Placeholders substituted per element of every argv:
//   {worktree}  the integration worktree under test
//   {hidden}    the hidden test directory (spec.base_dir)
//   {build}     a scratch build directory the runner creates
//   {run_dir}   the run directory (empty when the caller does not pass one)
std::vector<std::string> expand_hidden_argv(const std::vector<std::string>& argv,
                                            const std::filesystem::path& worktree,
                                            const std::filesystem::path& hidden_dir,
                                            const std::filesystem::path& build_dir,
                                            const std::filesystem::path& run_dir = {});

struct HiddenTestResult {
  int tests = 0;
  int passed = 0;
  bool ok = true;  // false when a compile step or the runner itself failed
  std::string error;
  std::vector<std::string> log;  // one line per step outcome
};

// Runs the spec: compile steps in the worktree, then the test steps. A
// missing spec is not an error; the caller decides. Test steps run with the
// spec directory as the working directory for compile steps and the worktree
// for test steps.
HiddenTestResult run_hidden_tests(const HiddenTestSpec& spec,
                                  const std::filesystem::path& worktree,
                                  const std::filesystem::path& build_dir,
                                  process::ProcessExecutor& executor,
                                  std::chrono::milliseconds timeout,
                                  const std::filesystem::path& run_dir = {});

// Parses the last `{"tests": N, "passed": M}` line of the output; returns
// nullopt when the output does not carry one.
struct TestCounts {
  int tests = 0;
  int passed = 0;
};
bool parse_test_counts(std::string_view output, TestCounts* counts);

}  // namespace cf::eval
