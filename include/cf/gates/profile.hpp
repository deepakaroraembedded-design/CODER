// cf-gates: toolchain profiles (design section 6.7).
//
// A profile is a YAML file under profiles/ that declares the format, build,
// guardrail, test and coverage tools for one target language family. Profiles
// contain *trusted* commands only: lists of steps, each an argument list,
// executed by the gate runner with no shell in between (design section
// 6.7.3). Adding a language means adding a profile, not changing C++ code.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::gates {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kProfileParseFailed = "PROFILE_PARSE_FAILED";
inline constexpr const char* kProfileSchemaInvalid = "PROFILE_SCHEMA_INVALID";
inline constexpr const char* kProfileDuplicate = "PROFILE_DUPLICATE";
inline constexpr const char* kProfileNotFound = "PROFILE_NOT_FOUND";
inline constexpr const char* kNoMatchingToolchain = "PROFILE_NO_MATCH";
inline constexpr const char* kUnknownPlaceholder = "PROFILE_UNKNOWN_PLACEHOLDER";
}  // namespace error_codes

// One trusted command: an argument list, never a shell string.
struct GateStep {
  std::vector<std::string> argv;
};

struct GateSpec {
  std::vector<GateStep> steps;
};

struct LanguageSpec {
  std::string standard_var;  // e.g. CMAKE_CXX_STANDARD
  GateSpec guardrails;
  GateSpec format;
};

struct BuildSystemSpec {
  std::vector<GateStep> steps;
  std::vector<std::string> agent_commands;  // simple commands the broker may allow
};

struct TestFrameworkSpec {
  std::vector<GateStep> steps;
  std::string report;                 // e.g. "junit"; empty when none
  std::vector<std::string> languages; // empty = valid for every language
};

struct RuntimeSpec {
  std::vector<std::string> presets;  // e.g. [asan-ubsan]
  std::string tsan_when;             // e.g. "component.concurrent"
};

struct CoverageSpec {
  std::string tool;  // e.g. llvm-cov
  double line = 0.0;
  double branch = 0.0;
};

struct MutationSpec {
  std::string tool;  // e.g. mull
  double min_score = 0.0;
};

struct Profile {
  std::string id;
  std::string image;
  std::string codeintel_backend;  // "clang" or "tree-sitter"
  std::map<std::string, LanguageSpec, std::less<>> languages;
  std::map<std::string, BuildSystemSpec, std::less<>> build_systems;
  std::map<std::string, TestFrameworkSpec, std::less<>> test_frameworks;
  std::map<std::string, GateSpec, std::less<>> common;  // gate name -> steps
  RuntimeSpec runtime;
  CoverageSpec coverage;
  MutationSpec mutation;
  std::vector<std::string> exec_from_source;  // exact paths (design 6.8.1)
};

Result<Profile> load_profile_string(std::string_view text);
Result<Profile> load_profile_file(const std::filesystem::path& path);

class ProfileRegistry {
 public:
  // Duplicate profile ids are rejected.
  Status add(Profile profile);

  Result<const Profile*> find(std::string_view id) const;

  // Loads every *.yaml / *.yml file in dir, sorted by filename for
  // determinism. A file that fails to load fails the whole directory.
  static Result<ProfileRegistry> load_dir(const std::filesystem::path& dir);

  // FR-18: resolves a plan target combination to the profile that declares
  // the language, build system and test framework (and whose test framework
  // lists the language, when it lists any). First match in id order wins.
  Result<const Profile*> resolve(std::string_view language,
                                 std::string_view build_system,
                                 std::string_view test_framework) const;

  const std::map<std::string, Profile, std::less<>>& all() const;

 private:
  std::map<std::string, Profile, std::less<>> profiles_;
};

// Placeholder values for one gate invocation (design section 6.7.3).
struct StepContext {
  std::string src_dir;
  std::string build_dir;
  std::vector<std::string> changed_files;
  std::string standard;         // e.g. "c++20"
  std::string standard_number;  // e.g. "20"
  std::string standard_var;     // e.g. CMAKE_CXX_STANDARD
};

// Expands one trusted step into a concrete argument list. The placeholder
// `{changed_files...}` splices one argument per file; other placeholders
// substitute inside a single element, so a value can never become shell
// syntax. Unknown placeholders are rejected.
Result<std::vector<std::string>> expand_step(const GateStep& step,
                                             const StepContext& context);

// Derives the numeric language standard ("c++20" -> "20", "c11" -> "11").
// Returns an empty string when the standard has no digit run.
std::string standard_number(std::string_view standard);

}  // namespace cf::gates
