// cf-codeintel: the tree-sitter back end (design section 6.5).
//
// One parser per language grammar; each grammar ships an extractor that
// walks the parsed tree and records its symbols, call edges and includes
// into the shared SymbolGraph. The grammars are vendored under
// third_party/tree-sitter-* (the vcpkg ports' prebuilt CLI cannot run on
// the build host, see third_party/VERSIONS); another language slots in as
// a grammar plus an extractor plus a factory below.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

#include "cf/codeintel/backend.hpp"

namespace cf::codeintel {

namespace detail {

// Per-file extraction state threaded through a grammar's tree walk.
struct Extraction {
  std::string_view source;
  std::string path;  // root-relative
  SymbolGraph* graph;
  std::string current_caller;  // the enclosing function/method, empty at file scope
  std::string current_scope;   // the enclosing class name, when the grammar
                               // tracks one (Python methods)
};

// A grammar's extractor: walks the parsed tree of one file.
using ExtractFn = void (*)(TSNode root, Extraction& extraction);

}  // namespace detail

class TreeSitterBackend final : public LanguageBackend {
 public:
  // `ts_language` is the grammar's entry point (e.g. tree_sitter_c());
  // `extensions` are the source file extensions it applies to (".c", ".h").
  TreeSitterBackend(const TSLanguage* ts_language, std::string language_id,
                    std::vector<std::string> extensions, detail::ExtractFn extract);
  ~TreeSitterBackend() override;
  TreeSitterBackend(const TreeSitterBackend&) = delete;
  TreeSitterBackend& operator=(const TreeSitterBackend&) = delete;

  std::string_view language() const override;
  Result<SymbolGraph> index(const std::filesystem::path& root) override;
  Result<SymbolGraph> update(const SymbolGraph& graph,
                             const std::vector<std::filesystem::path>& changed) override;
  Result<std::vector<Diagnostic>> check_architecture(
      const SymbolGraph& graph, const ArchitectureRules& rules) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The vendored grammars (third_party/tree-sitter-*, pinned in VERSIONS).
std::unique_ptr<LanguageBackend> make_c_backend();       // ".c", ".h"
std::unique_ptr<LanguageBackend> make_python_backend();  // ".py"
std::unique_ptr<LanguageBackend> make_go_backend();      // ".go"

}  // namespace cf::codeintel
