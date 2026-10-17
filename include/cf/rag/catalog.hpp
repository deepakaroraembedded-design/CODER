// cf-rag: the curated pattern catalog (design section 6.2).
//
// One YAML file per approved pattern. The catalog is the authoritative source
// for `get_pattern` and is also indexed into the retrieval collection as
// Pattern documents, so pattern queries through search_guidelines find it.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/rag/types.hpp"

namespace cf::rag {

// One pattern catalog entry (design section 6.2).
struct PatternEntry {
  std::string id;    // e.g. PAT-STRATEGY
  std::string name;  // e.g. Strategy
  std::string level; // system | component
  std::vector<std::string> use_when;
  std::vector<std::string> avoid_when;
  std::string company_notes;
  std::string reference_impl;
  // One conformance check id per language that has a check (design section
  // 6.5), e.g. {"cpp": "cf-pattern-strategy"}.
  std::map<std::string, std::string, std::less<>> conformance_checks;
  std::string source_path;

  // The catalog entry rendered as Markdown (the document body).
  std::string to_markdown() const;
};

Result<PatternEntry> parse_pattern_entry(std::string_view yaml,
                                         const std::string& source_path);

// Loads every *.yaml / *.yml file in dir, sorted by filename. A file that
// fails to parse fails the whole catalog; duplicate ids are rejected.
Result<std::vector<PatternEntry>> load_pattern_catalog(
    const std::filesystem::path& dir);

// Indexes a catalog entry as a Pattern document (status active, scope
// company) so the retrieval tools find it.
Document pattern_document(const PatternEntry& entry);

}  // namespace cf::rag
