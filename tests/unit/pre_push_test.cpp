// Unit tests for the pre-push checks (design section 10.4.6).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/github/pre_push.hpp"

namespace {

using cf::github::check_pre_push;
using cf::github::PrePushConfig;
using cf::github::PrePushFinding;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_prepush_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

class PrePushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));
    // An initial commit so every later commit has a parent to diff against.
    write_file(dir_ / "repo" / "README.md", "initial\n");
    cf::git::CommitTrailers init;
    init.run_id = "R12";
    init.op_id = "op_init";
    ASSERT_TRUE(repository_
                    ->commit_paths(dir_ / "repo", {"README.md"}, "chore: init", init)
                    .has_value());
  }

  void TearDown() override {
    repository_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string commit(const std::vector<std::string>& paths, const std::string& message,
                     const cf::git::CommitTrailers& trailers) {
    auto sha =
        repository_->commit_paths(dir_ / "repo", paths, message, trailers);
    EXPECT_TRUE(sha.has_value()) << sha.error().message;
    return sha.has_value() ? *sha : std::string();
  }

  bool has_finding(const std::vector<PrePushFinding>& findings,
                   const std::string& check) {
    for (const auto& finding : findings) {
      if (finding.check == check) {
        return true;
      }
    }
    return false;
  }

  std::filesystem::path dir_;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<cf::git::Repository> repository_;
};

TEST_F(PrePushTest, CleanCommitPasses) {
  write_file(dir_ / "repo" / "src" / "a.cpp", "int a() { return 0; }\n");
  cf::git::CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.milestone_id = "M3";
  trailers.op_id = "op_1";
  const std::string sha = commit({"src/a.cpp"}, "feat: a", trailers);
  auto findings = check_pre_push(*repository_, "cf/R12/M3", sha);
  ASSERT_TRUE(findings.has_value()) << findings.error().message;
  EXPECT_TRUE(findings->empty());
}

TEST_F(PrePushTest, BranchPolicyIsEnforced) {
  write_file(dir_ / "repo" / "src" / "a.cpp", "x\n");
  cf::git::CommitTrailers trailers;
  trailers.op_id = "op_1";
  const std::string sha = commit({"src/a.cpp"}, "feat: a", trailers);
  auto findings = check_pre_push(*repository_, "main", sha);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(has_finding(*findings, "branch-policy"));
}

TEST_F(PrePushTest, ProtectedPathsAreRejected) {
  write_file(dir_ / "repo" / "CODEOWNERS", "* @team\n");
  cf::git::CommitTrailers trailers;
  trailers.op_id = "op_1";
  const std::string sha = commit({"CODEOWNERS"}, "chore: owners", trailers);
  auto findings = check_pre_push(*repository_, "cf/R12/M3", sha);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(has_finding(*findings, "protected-path"));

  write_file(dir_ / "repo" / ".github" / "workflows" / "ci.yml", "on: push\n");
  cf::git::CommitTrailers trailers2;
  trailers2.op_id = "op_2";
  const std::string sha2 = commit({".github/workflows/ci.yml"}, "ci: change", trailers2);
  auto findings2 = check_pre_push(*repository_, "cf/R12/M3", sha2);
  ASSERT_TRUE(findings2.has_value());
  EXPECT_TRUE(has_finding(*findings2, "protected-path"));
}

TEST_F(PrePushTest, BinaryFilesAreRejected) {
  std::string binary = "abc";
  binary.push_back('\0');
  binary += "def";
  write_file(dir_ / "repo" / "asset.bin", binary);
  cf::git::CommitTrailers trailers;
  trailers.op_id = "op_1";
  const std::string sha = commit({"asset.bin"}, "feat: asset", trailers);
  auto findings = check_pre_push(*repository_, "cf/R12/M3", sha);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(has_finding(*findings, "binary-file"));
}

TEST_F(PrePushTest, OversizedFilesAreRejected) {
  write_file(dir_ / "repo" / "big.txt", "some content\n");
  cf::git::CommitTrailers trailers;
  trailers.op_id = "op_1";
  const std::string sha = commit({"big.txt"}, "feat: big", trailers);
  PrePushConfig config;
  config.max_file_mb = 0;  // any non-empty file exceeds the limit
  auto findings = check_pre_push(*repository_, "cf/R12/M3", sha, config);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(has_finding(*findings, "file-size"));
}

TEST_F(PrePushTest, SecretsAreRejected) {
  write_file(dir_ / "repo" / "config.env",
             "AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE\n");
  cf::git::CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.milestone_id = "M3";
  trailers.op_id = "op_1";
  const std::string sha = commit({"config.env"}, "chore: config", trailers);
  auto findings = check_pre_push(*repository_, "cf/R12/M3", sha);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(has_finding(*findings, "secret"));
}

TEST_F(PrePushTest, NewDependenciesAreCheckedAgainstTheAllowList) {
  auto database = cf::github::LicenseDatabase::load_string(
      "licenses:\n  fmt: MIT\n  gpl-thing: GPL-3.0-only\n");
  ASSERT_TRUE(database.has_value());
  PrePushConfig config;
  config.licenses = &*database;

  // The first commit adds an allowed and a disallowed dependency.
  write_file(dir_ / "repo" / "vcpkg.json",
             R"({"name": "x", "dependencies": ["fmt", "gpl-thing"]})");
  cf::git::CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.milestone_id = "M3";
  trailers.op_id = "op_deps";
  const std::string first = commit({"vcpkg.json"}, "feat: deps", trailers);
  auto findings = check_pre_push(*repository_, "cf/R12/M3", first, config);
  ASSERT_TRUE(findings.has_value());
  ASSERT_EQ(findings->size(), 1u);
  EXPECT_EQ((*findings)[0].check, "license");
  EXPECT_NE((*findings)[0].message.find("gpl-thing"), std::string::npos);

  // Adding only an allowed dependency afterwards is clean.
  write_file(dir_ / "repo" / "vcpkg.json",
             R"({"name": "x", "dependencies": ["fmt", "gpl-thing", "zlib"]})");
  auto database2 = cf::github::LicenseDatabase::load_string(
      "licenses:\n  fmt: MIT\n  gpl-thing: GPL-3.0-only\n  zlib: Zlib\n");
  ASSERT_TRUE(database2.has_value());
  config.licenses = &*database2;
  cf::git::CommitTrailers trailers2;
  trailers2.run_id = "R12";
  trailers2.milestone_id = "M3";
  trailers2.op_id = "op_deps2";
  const std::string second = commit({"vcpkg.json"}, "feat: add zlib", trailers2);
  auto findings2 = check_pre_push(*repository_, "cf/R12/M3", second, config);
  ASSERT_TRUE(findings2.has_value());
  for (const auto& finding : *findings2) {
    EXPECT_NE(finding.check, "license") << finding.message;
  }
}

TEST_F(PrePushTest, MissingTrailersAreRejected) {
  write_file(dir_ / "repo" / "src" / "a.cpp", "x\n");
  cf::git::CommitTrailers trailers;  // no run, milestone or op
  const std::string sha = commit({"src/a.cpp"}, "feat: a", trailers);
  auto findings = check_pre_push(*repository_, "cf/R12/M3", sha);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(has_finding(*findings, "trailer"));
}

}  // namespace
