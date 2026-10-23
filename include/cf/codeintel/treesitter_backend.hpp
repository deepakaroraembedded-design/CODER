// cf-codeintel: the tree-sitter back end (design section 6.5).
//
// One parser per language grammar; the grammar's extracted symbols, call
// edges and includes feed the shared SymbolGraph. The C grammar is vendored
// under third_party/tree-sitter-c (the vcpkg port's prebuilt CLI cannot run
// on the build host, see third_party/VERSIONS); other languages slot in by
// constructing the back end with their TSLanguage.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cf/codeintel/backend.hpp"

// tree-sitter's api.h typedefs `struct TSLanguage` — forward-declared here
// so the public header does not expose the tree-sitter headers.
typedef struct TSLanguage TSLanguage;

namespace cf::codeintel {

class TreeSitterBackend final : public LanguageBackend {
 public:
  // `ts_language` is the grammar's entry point (e.g. tree_sitter_c());
  // `extensions` are the source file extensions it applies to (".c", ".h").
  TreeSitterBackend(const TSLanguage* ts_language, std::string language_id,
                    std::vector<std::string> extensions);
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

// The C grammar (third_party/tree-sitter-c): indexes ".c" and ".h" files.
std::unique_ptr<LanguageBackend> make_c_backend();

}  // namespace cf::codeintel
