// cf-codeintel: the symbol index (design section 6.5, "Symbol index").
//
// The parsed symbol graph persisted in SQLite, stored per file so updates
// are incremental: a changed file's rows are replaced and every other file
// is left alone. The MCP tools find_symbol and get_callers (design section
// 9.1) read from this index.
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "cf/codeintel/types.hpp"

namespace cf::codeintel {

// Everything one source file contributed to the index.
struct FileIndex {
  FileRecord record;
  std::vector<Symbol> symbols;
  std::vector<CallEdge> calls;
  std::vector<IncludeEdge> includes;
};

class SymbolIndex {
 public:
  // Opens (creating when missing) the index database and its schema.
  static Result<std::unique_ptr<SymbolIndex>> open(const std::filesystem::path& db_path);

  ~SymbolIndex();
  SymbolIndex(const SymbolIndex&) = delete;
  SymbolIndex& operator=(const SymbolIndex&) = delete;
  SymbolIndex(SymbolIndex&&) noexcept;
  SymbolIndex& operator=(SymbolIndex&&) noexcept;

  // Atomically replaces everything recorded for one file.
  Status replace_file(const FileIndex& file);

  // Drops everything recorded for one file (deleted sources).
  Status remove_file(std::string_view path);

  // Exact match on the unqualified or the qualified name; definitions sort
  // before declarations, then by file and line.
  Result<std::vector<Symbol>> find_symbol(std::string_view name) const;

  // Every recorded call to the symbol (matched on the callee's unqualified
  // or qualified name), ordered by file and line.
  Result<std::vector<CallEdge>> get_callers(std::string_view symbol) const;

  // Index metadata (e.g. the commit the index reflects, so an incremental
  // refresh can diff against it). get_meta returns nullopt for a key that
  // was never set.
  Status set_meta(std::string_view key, std::string_view value);
  Result<std::optional<std::string>> get_meta(std::string_view key) const;

 private:
  struct Impl;
  explicit SymbolIndex(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cf::codeintel
