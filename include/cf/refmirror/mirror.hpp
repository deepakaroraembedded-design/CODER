// cf-refmirror: the mirroring job (design section 6.3.1).
//
// For each approved set: fetch, verify the checksum, clean the content,
// scan for injected instructions, write the cleaned Markdown (with front
// matter, so cf-mcp --docs can load it) and the raw snapshot for audit, and
// report per-set status. A set that fails checksum or injection review is not
// written; the job alerts instead of indexing it.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/common/result.hpp"
#include "cf/llm/client.hpp"
#include "cf/refmirror/manifest.hpp"

namespace cf::refmirror {

struct MirrorOptions {
  std::filesystem::path out_dir;
  std::chrono::milliseconds timeout{120000};
  // Used to unpack reference archives with the system tar/unzip binaries.
  // A local executor is created when this is null.
  process::ProcessExecutor* executor = nullptr;
};

struct SetReport {
  std::string id;
  std::string version;
  // ok | checksum_mismatch | injection_found | fetch_failed | unsupported
  std::string status;
  std::string detail;
  std::size_t documents = 0;
};

struct MirrorReport {
  std::vector<SetReport> sets;

  bool ok() const;
  std::string to_json() const;
};

// Fetches every set in the manifest. `only` (optional) limits the run to one
// set id. The report is also written to <out_dir>/report.json.
Result<MirrorReport> run_mirror(const Manifest& manifest, const MirrorOptions& options,
                                llm::HttpTransport& transport,
                                const std::string& only = "");

}  // namespace cf::refmirror
