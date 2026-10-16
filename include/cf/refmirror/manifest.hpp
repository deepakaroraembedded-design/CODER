// cf-refmirror: reference documentation manifest (design section 6.3.1).
//
// The manifest lists the approved documentation sets. Only the URLs and
// repositories listed there are ever contacted; a change goes through a pull
// request approved by a tech lead.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::refmirror {

namespace error_codes {
inline constexpr const char* kManifestInvalid = "REFMANIFEST_INVALID";
}  // namespace error_codes

struct ReferenceSet {
  std::string id;
  std::string version;
  std::string source;   // URL (http/https) or a local file path
  std::string sha256;   // required for remote sources; empty = local dev only
  std::string license;
  std::vector<std::string> extract;  // archive paths (archives unsupported yet)
  std::string generate;              // doxygen, ... (generators not implemented)
  std::string repo;
  std::string ref;
};

struct Manifest {
  std::vector<ReferenceSet> sets;

  const ReferenceSet* find(std::string_view id) const;
};

Result<Manifest> load_manifest_string(std::string_view text);
Result<Manifest> load_manifest_file(const std::filesystem::path& path);

}  // namespace cf::refmirror
