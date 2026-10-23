// Unit tests for CodeIntelSync: the run's symbol index tracking the
// integration branch, incrementally per commit (design section 6.5).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/codeintel/symbol_index.hpp"
#include "cf/codeintel/treesitter_backend.hpp"
#include "cf/orch/codeintel_sync.hpp"

namespace {

using cf::codeintel::SymbolIndex;
using cf::orch::CodeIntelSync;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_codeintel_sync_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const char* kUtilC = R"c(int add(int a, int b) {
  return a + b;
}
)c";

class CodeIntelSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));

    write_file(dir_ / "repo" / "util.c", kUtilC);
    commit({"util.c"}, "chore: initial util");

    backend_ = cf::codeintel::make_c_backend();
    auto sync = CodeIntelSync::open(*backend_, *repository_, dir_ / "codeintel.db",
                                    dir_ / "repo");
    ASSERT_TRUE(sync.has_value()) << sync.error().message;
    sync_ = std::move(*sync);

    // A reader handle on the same database, the way cf-mcp reads it.
    auto index = SymbolIndex::open(dir_ / "codeintel.db");
    ASSERT_TRUE(index.has_value()) << index.error().message;
    index_ = std::move(*index);
  }

  void TearDown() override {
    index_.reset();
    sync_.reset();
    backend_.reset();
    repository_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void commit(const std::vector<std::string>& paths, const std::string& message) {
    cf::git::CommitTrailers trailers;
    trailers.run_id = "R12";
    trailers.op_id = "op_" + std::to_string(++commits_);
    ASSERT_TRUE(
        repository_->commit_paths(dir_ / "repo", paths, message, trailers).has_value());
  }

  std::string head() {
    auto sha = repository_->head_sha(dir_ / "repo");
    EXPECT_TRUE(sha.has_value()) << sha.error().message;
    return *sha;
  }

  std::filesystem::path dir_;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<cf::git::Repository> repository_;
  std::unique_ptr<cf::codeintel::LanguageBackend> backend_;
  std::unique_ptr<CodeIntelSync> sync_;
  std::unique_ptr<SymbolIndex> index_;
  int commits_ = 0;
};

TEST_F(CodeIntelSyncTest, FirstRefreshIndexesTheTree) {
  auto refreshed = sync_->refresh_to(head());
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
  EXPECT_EQ(*refreshed, 1u);  // util.c

  auto hits = index_->find_symbol("add");
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ(hits->at(0).file, "util.c");

  auto recorded = sync_->indexed_commit();
  ASSERT_TRUE(recorded.has_value());
  ASSERT_TRUE(recorded->has_value());
  EXPECT_EQ(**recorded, head());
}

TEST_F(CodeIntelSyncTest, RefreshToTheSameCommitIsANoOp) {
  ASSERT_TRUE(sync_->refresh_to(head()).has_value());
  auto again = sync_->refresh_to(head());
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, 0u);
}

TEST_F(CodeIntelSyncTest, IncrementalRefreshReindexesOnlyChangedFiles) {
  ASSERT_TRUE(sync_->refresh_to(head()).has_value());

  write_file(dir_ / "repo" / "util.c",
             std::string(kUtilC) + "\nint twice(int v) {\n  return add(v, v);\n}\n");
  write_file(dir_ / "repo" / "untouched.c", "int other(void) { return 1; }\n");
  // untouched.c is committed in the same commit but was already indexed below.
  commit({"util.c", "untouched.c"}, "feat: add twice");

  auto refreshed = sync_->refresh_to(head());
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
  EXPECT_EQ(*refreshed, 2u);  // the two changed files, not the whole tree

  auto twice = index_->find_symbol("twice");
  ASSERT_TRUE(twice.has_value());
  ASSERT_EQ(twice->size(), 1u);
  auto add = index_->find_symbol("add");
  ASSERT_TRUE(add.has_value());
  EXPECT_EQ(add->size(), 1u);  // replaced, not duplicated
  auto callers = index_->get_callers("add");
  ASSERT_TRUE(callers.has_value());
  ASSERT_EQ(callers->size(), 1u);
  EXPECT_EQ(callers->at(0).caller, "twice");
}

TEST_F(CodeIntelSyncTest, DeletedFilesLeaveTheIndex) {
  ASSERT_TRUE(sync_->refresh_to(head()).has_value());

  ASSERT_TRUE(repository_->command({"rm", "util.c"}).has_value());
  // The deletion is staged; committing with any path carries it.
  write_file(dir_ / "repo" / "README.md", "x\n");
  commit({"README.md"}, "chore: drop util.c");

  auto refreshed = sync_->refresh_to(head());
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
  EXPECT_EQ(*refreshed, 1u);  // util.c removed (README.md is not indexed)

  auto hits = index_->find_symbol("add");
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());
}

TEST_F(CodeIntelSyncTest, RenamesMoveTheIndexRows) {
  ASSERT_TRUE(sync_->refresh_to(head()).has_value());

  ASSERT_TRUE(repository_->command({"mv", "util.c", "math.c"}).has_value());
  commit({"math.c"}, "refactor: rename util.c to math.c");

  auto refreshed = sync_->refresh_to(head());
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;

  auto hits = index_->find_symbol("add");
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ(hits->at(0).file, "math.c");  // not stale under util.c
}

TEST_F(CodeIntelSyncTest, AFailedDiffFallsBackToAFullReindex) {
  ASSERT_TRUE(sync_->refresh_to(head()).has_value());

  // A commit that is not in the repository makes the diff fail; the sync
  // re-indexes the whole tree instead of failing the run.
  auto refreshed = sync_->refresh_to("0000000000000000000000000000000000000000");
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().message;
  EXPECT_EQ(*refreshed, 1u);
  auto hits = index_->find_symbol("add");
  ASSERT_TRUE(hits.has_value());
  EXPECT_EQ(hits->size(), 1u);
}

}  // namespace
