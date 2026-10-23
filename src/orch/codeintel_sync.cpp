#include "cf/orch/codeintel_sync.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#include "cf/codeintel/indexer.hpp"

namespace cf::orch {
namespace {

// The SymbolIndex meta key recording the commit the index reflects.
constexpr const char* kIndexedCommit = "indexed_commit";

}  // namespace

CodeIntelSync::CodeIntelSync(codeintel::LanguageBackend& backend,
                             git::Repository& repository,
                             std::filesystem::path db_path, std::filesystem::path tree,
                             std::unique_ptr<codeintel::SymbolIndex> index)
    : backend_(&backend),
      repository_(&repository),
      db_path_(std::move(db_path)),
      tree_(std::move(tree)),
      index_(std::move(index)) {}

CodeIntelSync::~CodeIntelSync() = default;

Result<std::unique_ptr<CodeIntelSync>> CodeIntelSync::open(
    codeintel::LanguageBackend& backend, git::Repository& repository,
    std::filesystem::path db_path, std::filesystem::path tree) {
  auto index = codeintel::SymbolIndex::open(db_path);
  if (!index) {
    return tl::make_unexpected(index.error());
  }
  return std::unique_ptr<CodeIntelSync>(new CodeIntelSync(
      backend, repository, std::move(db_path), std::move(tree), std::move(*index)));
}

Result<std::optional<std::string>> CodeIntelSync::indexed_commit() const {
  return index_->get_meta(kIndexedCommit);
}

Result<std::size_t> CodeIntelSync::refresh_to(const std::string& commit) {
  auto recorded = index_->get_meta(kIndexedCommit);
  if (!recorded) {
    return tl::make_unexpected(recorded.error());
  }
  if (recorded->has_value() && **recorded == commit) {
    return 0;  // already current
  }

  // Incremental path (design section 6.5): re-index only what changed since
  // the recorded commit. A rewritten history makes the diff fail; fall back
  // to a full re-index, which is always correct.
  if (recorded->has_value()) {
    auto changes = repository_->diff(**recorded, commit);
    if (changes.has_value()) {
      std::vector<std::filesystem::path> changed;
      for (const auto& change : *changes) {
        changed.emplace_back(change.path);
        if (!change.old_path.empty()) {
          changed.emplace_back(change.old_path);  // renamed away: drop it
        }
      }
      auto updated =
          codeintel::update_files(*backend_, *index_, tree_, changed);
      if (!updated) {
        return tl::make_unexpected(updated.error());
      }
      auto stamped = index_->set_meta(kIndexedCommit, commit);
      if (!stamped) {
        return tl::make_unexpected(stamped.error());
      }
      return *updated;
    }
    spdlog::warn("codeintel: cannot diff {}..{} ({}); re-indexing the tree",
                 **recorded, commit, changes.error().message);
  }

  auto indexed = codeintel::index_tree(*backend_, *index_, tree_);
  if (!indexed) {
    return tl::make_unexpected(indexed.error());
  }
  auto stamped = index_->set_meta(kIndexedCommit, commit);
  if (!stamped) {
    return tl::make_unexpected(stamped.error());
  }
  return *indexed;
}

}  // namespace cf::orch
