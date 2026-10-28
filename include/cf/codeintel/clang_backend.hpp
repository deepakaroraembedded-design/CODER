// cf-codeintel: the Clang LibTooling back end for C and C++ (design section
// 6.5). Built only when LLVM/Clang 17 is available (CF_HAS_CLANG).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cf/codeintel/backend.hpp"

namespace cf::codeintel {

// `language` is "c" or "cpp"; `extensions` selects the source files the
// backend indexes. Falls back to the tree-sitter back end when a translation
// unit cannot be parsed.
std::unique_ptr<LanguageBackend> make_clang_backend(
    std::string language, std::vector<std::string> extensions);

}  // namespace cf::codeintel
