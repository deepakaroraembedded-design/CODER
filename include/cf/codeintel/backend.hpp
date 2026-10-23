// cf-codeintel: the language back-end interface (design section 6.5).
//
// One back end per language family (Clang LibTooling for C/C++, tree-sitter
// for the other languages) behind a common interface, so the symbol index,
// context packer and MCP tools work identically for every language.
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "cf/codeintel/types.hpp"

namespace cf::codeintel {

class LanguageBackend {
 public:
  virtual ~LanguageBackend() = default;
  virtual std::string_view language() const = 0;  // "cpp", "python", ...

  // Parses every source file the back end recognises under `root`.
  virtual Result<SymbolGraph> index(const std::filesystem::path& root) = 0;

  // Re-parses only `changed` files (root-relative paths) and returns a graph
  // covering just those files. Files that no longer exist on disk are simply
  // absent from the result; the caller drops them from the index.
  virtual Result<SymbolGraph> update(const SymbolGraph& graph,
                                     const std::vector<std::filesystem::path>& changed) = 0;

  // Architecture layering and dependency rules from the architecture
  // document (design section 6.5, "Architecture rules").
  virtual Result<std::vector<Diagnostic>> check_architecture(
      const SymbolGraph& graph, const ArchitectureRules& rules) = 0;
};

class BackendRegistry {
 public:
  void add(std::unique_ptr<LanguageBackend> backend);
  Result<LanguageBackend*> for_language(std::string_view language) const;

 private:
  std::vector<std::unique_ptr<LanguageBackend>> backends_;
};

// The layering check every back end's check_architecture shares: a file
// under a rule's `from_prefix` may not include a path under its
// `forbidden_prefix`. System includes (<...>) are not project paths and are
// never violations.
std::vector<Diagnostic> check_layer_rules(const SymbolGraph& graph,
                                          const ArchitectureRules& rules);

}  // namespace cf::codeintel
