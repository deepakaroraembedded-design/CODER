// cf-refmirror: version matching for the offline reference mirror (design
// section 6.3.1).
//
// A run uses the documentation version pinned by the target project's
// dependency files (vcpkg.json, requirements.txt, package.json). A set whose
// mirrored version differs is skipped, never silently replaced by another
// version; lookup_reference then reports "not covered".
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/refmirror/manifest.hpp"

namespace cf::refmirror {

struct SkippedSet {
  std::string id;
  std::string mirrored_version;
  std::string pinned_version;
};

struct VersionReport {
  // set id -> the version pinned by the project (empty when the set is not
  // a project dependency and keeps its mirrored version).
  std::map<std::string, std::string, std::less<>> pinned;
  std::vector<SkippedSet> skipped;
};

// Reads exact versions from the dependency files in a project checkout.
// Names are lower-cased; entries without an exact version are ignored.
Result<std::map<std::string, std::string, std::less<>>> dependency_versions(
    const std::filesystem::path& project_dir);

// Returns the manifest restricted to sets the project pins to the mirrored
// version (or does not pin at all). The report records the included set
// versions and the skipped mismatches.
Manifest match_versions(const Manifest& manifest,
                        const std::map<std::string, std::string, std::less<>>& deps,
                        VersionReport& report);

}  // namespace cf::refmirror
