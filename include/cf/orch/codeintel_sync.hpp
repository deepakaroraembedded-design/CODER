// cf-orch: keeps the run's code intelligence index in step with the
// integration branch (design section 6.5, "updated incrementally per
// commit").
//
// The index tracks the integration worktree: it is built when a run starts
// and refreshed after every merge, so the agents' find_symbol/get_callers
// MCP tools (design section 9.1) answer against the code as integrated.
// Refreshes diff the recorded commit against the new head and re-index only
// the changed files; when there is no recorded commit (or history was
// rewritten underneath us) the whole tree is re-indexed.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "cf/codeintel/backend.hpp"
#include "cf/codeintel/symbol_index.hpp"
#include "cf/git/repository.hpp"

namespace cf::orch {

class CodeIntelSync {
 public:
  // Opens (creating when needed) the index at db_path. `tree` is the
  // integration worktree the indexed content is read from.
  static Result<std::unique_ptr<CodeIntelSync>> open(
      codeintel::LanguageBackend& backend, git::Repository& repository,
      std::filesystem::path db_path, std::filesystem::path tree);

  ~CodeIntelSync();
  CodeIntelSync(const CodeIntelSync&) = delete;
  CodeIntelSync& operator=(const CodeIntelSync&) = delete;

  // Brings the index to `commit` and returns how many files were (re-)
  // indexed; 0 when the index already reflects the commit. The worktree
  // must be checked out at `commit`.
  Result<std::size_t> refresh_to(const std::string& commit);

  // The commit the index currently reflects; nullopt when never indexed.
  Result<std::optional<std::string>> indexed_commit() const;

  const std::filesystem::path& db_path() const { return db_path_; }

 private:
  CodeIntelSync(codeintel::LanguageBackend& backend, git::Repository& repository,
                std::filesystem::path db_path, std::filesystem::path tree,
                std::unique_ptr<codeintel::SymbolIndex> index);

  codeintel::LanguageBackend* backend_;
  git::Repository* repository_;
  std::filesystem::path db_path_;
  std::filesystem::path tree_;
  std::unique_ptr<codeintel::SymbolIndex> index_;
};

}  // namespace cf::orch
