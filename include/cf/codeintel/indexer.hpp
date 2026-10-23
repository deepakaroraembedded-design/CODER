// cf-codeintel: glue between a language back end and the symbol index
// (design section 6.5, "updated incrementally per commit").
#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "cf/codeintel/backend.hpp"
#include "cf/codeintel/symbol_index.hpp"

namespace cf::codeintel {

// Indexes every source file the back end recognises under `root` into the
// symbol index, replacing whatever the index held for those files. Returns
// the number of files indexed.
Result<std::size_t> index_tree(LanguageBackend& backend, SymbolIndex& index,
                               const std::filesystem::path& root);

// Re-indexes only the `changed` files (root-relative paths); files that no
// longer exist on disk are dropped from the index. Returns the number of
// files re-indexed or removed.
Result<std::size_t> update_files(LanguageBackend& backend, SymbolIndex& index,
                                 const std::filesystem::path& root,
                                 const std::vector<std::filesystem::path>& changed);

}  // namespace cf::codeintel
