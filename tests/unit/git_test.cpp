// Unit tests for the git repository layer (design sections 6.8.3, 10.4.5).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/git/repository.hpp"

namespace {

using cf::git::CommitTrailers;
using cf::git::Repository;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_git_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

class GitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    executor_ = std::make_unique<cf::process::LocalProcessExecutor>();
    auto created = Repository::create(dir_ / "repo", *executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repo_ = std::make_unique<Repository>(std::move(*created));

    write_file(dir_ / "repo" / "README.md", "a\nb\nc\n");
    CommitTrailers trailers;
    trailers.run_id = "R12";
    trailers.op_id = "op_init";
    auto sha = repo_->commit_paths(dir_ / "repo", {"README.md"}, "chore: init", trailers);
    ASSERT_TRUE(sha.has_value()) << sha.error().message;
    initial_sha_ = *sha;
  }

  void TearDown() override {
    repo_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::process::LocalProcessExecutor> executor_;
  std::unique_ptr<Repository> repo_;
  std::string initial_sha_;
};

TEST_F(GitTest, OpenChecksForARepository) {
  auto opened = Repository::open(dir_ / "repo", *executor_);
  ASSERT_TRUE(opened.has_value()) << opened.error().message;

  const auto missing = Repository::open(dir_ / "nope", *executor_);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, cf::git::error_codes::kNotARepository);
}

TEST_F(GitTest, CreateWorksWithARelativePath) {
  // Regression: `git -C <cwd>` used to be resolved relative to the process
  // working directory, which is also <cwd>, so a relative path looked up
  // `<cwd>/<cwd>` and init failed.
  const std::filesystem::path relative =
      std::filesystem::relative(dir_ / "repo-rel", std::filesystem::current_path());
  ASSERT_FALSE(relative.is_absolute());

  auto created = Repository::create(relative, *executor_);
  ASSERT_TRUE(created.has_value()) << created.error().message;

  write_file(dir_ / "repo-rel" / "file.txt", "x\n");
  auto sha = created->commit_paths(relative, {"file.txt"}, "feat: relative", {});
  ASSERT_TRUE(sha.has_value()) << sha.error().message;
}

TEST_F(GitTest, CommitCarriesTrailers) {
  write_file(dir_ / "repo" / "src.cpp", "int main() { return 0; }\n");
  CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.milestone_id = "M3";
  trailers.requirements = {"REQ-010", "REQ-011"};
  trailers.agents = "coder=deepseek; reviewer=kimi; qa=glm";
  trailers.gates = "passed (report: artifact://runs/R12/M3/gates.json)";
  trailers.op_id = "op_abc";

  auto sha = repo_->commit_paths(dir_ / "repo", {"src.cpp"}, "feat: add main", trailers);
  ASSERT_TRUE(sha.has_value()) << sha.error().message;

  auto found = repo_->find_commit_by_trailer("HEAD", "CF-Op", "op_abc");
  ASSERT_TRUE(found.has_value()) << found.error().message;
  EXPECT_EQ(*found, *sha);
  auto head = repo_->head_sha(dir_ / "repo");
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(*head, *sha);
}

TEST_F(GitTest, CommitRejectsEscapingPaths) {
  const auto parent = repo_->commit_paths(dir_ / "repo", {"../evil.txt"}, "x", {});
  ASSERT_FALSE(parent.has_value());
  EXPECT_EQ(parent.error().code, cf::git::error_codes::kInvalidPath);

  const auto absolute = repo_->commit_paths(dir_ / "repo", {"/etc/passwd"}, "x", {});
  ASSERT_FALSE(absolute.has_value());
  EXPECT_EQ(absolute.error().code, cf::git::error_codes::kInvalidPath);

  const auto head = repo_->head_sha(dir_ / "repo");
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(*head, initial_sha_);
}

TEST_F(GitTest, CommitWithNoChangesFails) {
  const auto again = repo_->commit_paths(dir_ / "repo", {"README.md"}, "noop", {});
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, cf::git::error_codes::kNoChanges);
}

TEST_F(GitTest, WorktreeCreateCommitAndRemove) {
  const auto worktree = dir_ / "wt" / "coder";
  ASSERT_TRUE(repo_->create_worktree(worktree, "cf-local/R12/M3/coder").has_value());
  EXPECT_TRUE(std::filesystem::exists(worktree));

  write_file(worktree / "feature.cpp", "feature\n");
  CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.milestone_id = "M3";
  trailers.op_id = "op_wt";
  auto sha = repo_->commit_paths(worktree, {"feature.cpp"}, "feat: feature", trailers);
  ASSERT_TRUE(sha.has_value()) << sha.error().message;

  auto main_head = repo_->head_sha(dir_ / "repo", "main");
  ASSERT_TRUE(main_head.has_value());
  EXPECT_EQ(*main_head, initial_sha_);  // the main branch did not move

  auto exists = repo_->branch_exists("cf-local/R12/M3/coder");
  ASSERT_TRUE(exists.has_value());
  EXPECT_TRUE(*exists);

  ASSERT_TRUE(repo_->remove_worktree(worktree).has_value());
  EXPECT_FALSE(std::filesystem::exists(worktree));
}

TEST_F(GitTest, DiffReportsCountsAndStatus) {
  write_file(dir_ / "repo" / "README.md", "a\nB\nc\nd\n");  // 2 added, 1 removed
  write_file(dir_ / "repo" / "new.md", "x\ny\n");           // 2 added
  CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.op_id = "op_2";
  auto sha = repo_->commit_paths(dir_ / "repo", {"README.md", "new.md"}, "feat: change",
                                 trailers);
  ASSERT_TRUE(sha.has_value()) << sha.error().message;

  auto changes = repo_->diff(initial_sha_, *sha);
  ASSERT_TRUE(changes.has_value()) << changes.error().message;
  ASSERT_EQ(changes->size(), 2u);
  EXPECT_EQ((*changes)[0].path, "README.md");
  EXPECT_EQ((*changes)[0].status, 'M');
  EXPECT_EQ((*changes)[0].added_lines, 2u);
  EXPECT_EQ((*changes)[0].removed_lines, 1u);
  EXPECT_EQ((*changes)[1].path, "new.md");
  EXPECT_EQ((*changes)[1].status, 'A');
  EXPECT_EQ((*changes)[1].added_lines, 2u);
  EXPECT_EQ((*changes)[1].removed_lines, 0u);
}

TEST_F(GitTest, FileHashIsStableAndDetectsChanges) {
  auto before = repo_->file_hash(initial_sha_, "README.md");
  ASSERT_TRUE(before.has_value());

  write_file(dir_ / "repo" / "README.md", "changed\n");
  CommitTrailers trailers;
  trailers.op_id = "op_change";
  auto sha = repo_->commit_paths(dir_ / "repo", {"README.md"}, "feat: change", trailers);
  ASSERT_TRUE(sha.has_value());

  auto after = repo_->file_hash(*sha, "README.md");
  ASSERT_TRUE(after.has_value());
  EXPECT_NE(*before, *after);

  auto unchanged = repo_->file_hash(initial_sha_, "README.md");
  EXPECT_EQ(*unchanged, *before);

  auto missing = repo_->file_hash(initial_sha_, "nope.md");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, cf::git::error_codes::kNotFound);
}

TEST_F(GitTest, FindCommitByTrailerWalksBack) {
  write_file(dir_ / "repo" / "one.md", "1\n");
  CommitTrailers second;
  second.op_id = "op_second";
  ASSERT_TRUE(repo_->commit_paths(dir_ / "repo", {"one.md"}, "feat: one", second)
                  .has_value());
  write_file(dir_ / "repo" / "two.md", "2\n");
  CommitTrailers third;
  third.op_id = "op_third";
  ASSERT_TRUE(
      repo_->commit_paths(dir_ / "repo", {"two.md"}, "feat: two", third).has_value());

  auto found = repo_->find_commit_by_trailer("HEAD", "CF-Op", "op_init");
  ASSERT_TRUE(found.has_value()) << found.error().message;
  EXPECT_EQ(*found, initial_sha_);

  auto limited = repo_->find_commit_by_trailer("HEAD", "CF-Op", "op_init", 2);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(limited.error().code, cf::git::error_codes::kNotFound);

  auto unknown = repo_->find_commit_by_trailer("HEAD", "CF-Op", "op_missing");
  ASSERT_FALSE(unknown.has_value());
}

TEST_F(GitTest, MergeSquashIntegratesABranch) {
  const auto integration = dir_ / "wt" / "integration";
  ASSERT_TRUE(repo_->create_worktree(integration, "integration", "main").has_value());
  const auto feature = dir_ / "wt" / "feature";
  ASSERT_TRUE(
      repo_->create_worktree(feature, "cf-local/R12/M3/coder", "main").has_value());

  write_file(feature / "feature.cpp", "feature\n");
  CommitTrailers feature_trailers;
  feature_trailers.run_id = "R12";
  feature_trailers.milestone_id = "M3";
  feature_trailers.op_id = "op_feat";
  ASSERT_TRUE(repo_->commit_paths(feature, {"feature.cpp"}, "feat: feature",
                                  feature_trailers)
                  .has_value());

  CommitTrailers merge_trailers;
  merge_trailers.run_id = "R12";
  merge_trailers.milestone_id = "M3";
  merge_trailers.op_id = "op_merge";
  merge_trailers.squashed_from = "cf-local/R12/M3/coder";
  auto merged = repo_->merge_squash(integration, "cf-local/R12/M3/coder",
                                    "feat: feature (#M3)", merge_trailers);
  ASSERT_TRUE(merged.has_value()) << merged.error().message;
  EXPECT_TRUE(std::filesystem::exists(integration / "feature.cpp"));

  auto found = repo_->find_commit_by_trailer("integration", "CF-Op", "op_merge");
  ASSERT_TRUE(found.has_value()) << found.error().message;
  EXPECT_EQ(*found, *merged);
}

TEST_F(GitTest, MergeConflictIsReportedAndAbortable) {
  const auto integration = dir_ / "wt" / "integration";
  ASSERT_TRUE(repo_->create_worktree(integration, "integration", "main").has_value());
  write_file(integration / "conflict.txt", "integration side\n");
  CommitTrailers integration_trailers;
  integration_trailers.op_id = "op_int";
  ASSERT_TRUE(repo_->commit_paths(integration, {"conflict.txt"}, "feat: integration",
                                  integration_trailers)
                  .has_value());

  const auto feature = dir_ / "wt" / "feature";
  ASSERT_TRUE(
      repo_->create_worktree(feature, "cf-local/R12/M4/coder", "main").has_value());
  write_file(feature / "conflict.txt", "feature side\n");
  CommitTrailers feature_trailers;
  feature_trailers.op_id = "op_feat";
  ASSERT_TRUE(repo_->commit_paths(feature, {"conflict.txt"}, "feat: feature",
                                  feature_trailers)
                  .has_value());

  auto merged =
      repo_->merge_squash(integration, "cf-local/R12/M4/coder", "merge M4", {});
  ASSERT_FALSE(merged.has_value());
  EXPECT_EQ(merged.error().code, cf::git::error_codes::kMergeConflict);

  ASSERT_TRUE(repo_->abort_merge(integration).has_value());
  EXPECT_EQ(read_file(integration / "conflict.txt"), "integration side\n");
}

}  // namespace
