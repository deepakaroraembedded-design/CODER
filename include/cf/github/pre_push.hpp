// cf-github: pre-push checks (design section 10.4.6).
//
// The orchestrator runs these on the exact commit it is about to push and
// refuses to push if any fail. Implemented here: target branch, protected
// paths, size and binaries, required trailers. (Gates, secrets and licences
// are checked by their own subsystems.)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/git/repository.hpp"
#include "cf/github/licenses.hpp"

namespace cf::github {

struct PrePushConfig {
  std::string branch_prefix = "cf/";
  std::vector<std::string> protected_paths = {".github/**", "CODEOWNERS", "cf.yaml",
                                              "deploy/**"};
  uint64_t max_file_mb = 5;
  bool allow_binaries = false;
  std::vector<std::string> required_trailers = {"CF-Run", "CF-Milestone", "CF-Op"};
  // When set, new dependencies in changed manifests are checked against the
  // database and its allow-list (design section 10.4.6 check 6).
  const LicenseDatabase* licenses = nullptr;
  LicensePolicy license_policy;
};

struct PrePushFinding {
  std::string check;    // branch-policy | protected-path | file-size | binary-file | trailer
  std::string message;
};

// Returns every finding; an empty vector means the push may proceed.
Result<std::vector<PrePushFinding>> check_pre_push(git::Repository& repository,
                                                   const std::string& branch,
                                                   const std::string& commit,
                                                   const PrePushConfig& config = {});

}  // namespace cf::github
