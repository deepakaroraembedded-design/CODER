// cf-github: dependency licence checks (design sections 6.7.1 gate 10,
// 10.4.6 check 6).
//
// New dependencies introduced by a commit are extracted from the project's
// manifests and checked against an allow-list, using a local licence database
// (package -> SPDX id). A licence that is not on the list, or a package the
// database does not know, becomes a finding that needs a human.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::github {

namespace error_codes {
inline constexpr const char* kLicenseDbInvalid = "GITHUB_LICENSE_DB_INVALID";
}  // namespace error_codes

struct LicenseDatabase {
  std::map<std::string, std::string, std::less<>> licenses;  // package -> SPDX id

  static Result<LicenseDatabase> load_string(std::string_view yaml);
  static Result<LicenseDatabase> load_file(const std::filesystem::path& path);

  std::string lookup(std::string_view package) const;
};

struct LicensePolicy {
  std::vector<std::string> allowed = {"MIT",     "Apache-2.0", "BSD-2-Clause",
                                      "BSD-3-Clause", "ISC",  "BSL-1.0",
                                      "Zlib",    "Unlicense",  "0BSD"};
};

struct LicenseFinding {
  std::string package;
  std::string license;  // empty when unknown
  std::string reason;
};

// The dependencies declared by a manifest, keyed by filename:
//   vcpkg.json, requirements.txt, package.json, go.mod
// Unknown manifest names yield no dependencies.
Result<std::vector<std::string>> dependencies_in_manifest(std::string_view filename,
                                                          std::string_view content);

// Checks every dependency against the database and the allow-list.
std::vector<LicenseFinding> check_licenses(
    const std::vector<std::string>& dependencies, const LicenseDatabase& database,
    const LicensePolicy& policy = {});

}  // namespace cf::github
