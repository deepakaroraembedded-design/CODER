// cf-rag: document loader with YAML front matter (design section 6.3,
// ingestion).
//
// Company documents are Markdown files with metadata in front matter:
//
//   ---
//   doc_id: CG
//   version: "3.2"
//   type: guideline
//   owner: engineering
//   classification: internal
//   scope: product          # company | product | component | language
//   scope_value: product-y
//   effective_from: 2026-01-01   # epoch seconds or YYYY-MM-DD
//   status: active
//   ---
//   # Heading
//   ...
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/rag/types.hpp"

namespace cf::rag {

// The body becomes document.text; source_path is the default citation base.
Result<Document> load_document_string(std::string_view text,
                                      const std::string& source_path);

// Loads every *.md / *.markdown file in dir, sorted by filename.
Result<std::vector<Document>> load_document_dir(const std::filesystem::path& dir);

}  // namespace cf::rag
