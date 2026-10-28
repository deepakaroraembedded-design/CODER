// Unit tests for branch retention cleanup (design section 10.4.9).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/cli/commands.hpp"
#include "cf/git/repository.hpp"
#include "cf/store/run_store.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_cleanup_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::string recent_iso_time(int64_t seconds_ago) {
  const std::time_t at = std::time(nullptr) - seconds_ago;
  std::tm utc{};
  gmtime_r(&at, &utc);
  char buffer[32] = {0};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

class CleanupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    now_ = cf::store::unix_now();

    cf::process::ProcessRequest bare;
    bare.argv = {"git", "init", "--bare", (dir_ / "remote.git").string()};
    bare.timeout = std::chrono::seconds(30);
    const auto remote = executor_.run(bare);
    ASSERT_TRUE(remote.launched && remote.exit_code == 0) << remote.stderr_text;

    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));
    write_file(dir_ / "repo" / "README.md", "initial\n");
    cf::git::CommitTrailers trailers;
    trailers.run_id = "R12";
    trailers.milestone_id = "M1";
    trailers.op_id = "op_init";
    auto initial =
        repository_->commit_paths(dir_ / "repo", {"README.md"}, "chore: init",
                                  trailers);
    ASSERT_TRUE(initial.has_value()) << initial.error().message;
    ASSERT_TRUE(repository_
                    ->command({"remote", "add", "origin",
                               (dir_ / "remote.git").string()})
                    .has_value());
    for (const char* branch :
         {"cf/R12/M1", "cf/R12/integration", "cf/R99/keep"}) {
      ASSERT_TRUE(repository_->command({"branch", branch, *initial}).has_value());
      ASSERT_TRUE(repository_->command({"push", "origin", branch}).has_value());
    }

    std::filesystem::create_directories(dir_ / "runs" / "R12");
    auto opened = cf::store::RunStore::open(dir_ / "runs" / "R12" / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Done";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());

    auto lease = store_->acquire_lease("R12", "test", std::chrono::seconds(60),
                                       now_);
    ASSERT_TRUE(lease.has_value());
    cf::store::NewOperation operation;
    operation.idempotency_key = "pr-create-R12";
    operation.kind = "pr_create";
    operation.input_json = "{}";
    operation.expected_json = "{}";
    ASSERT_TRUE(store_->record_intents("R12", {operation}, now_).has_value());
    auto recorded = store_->find_operation_by_key("pr-create-R12");
    ASSERT_TRUE(recorded.has_value()) << recorded.error().message;
    ASSERT_TRUE(store_
                    ->mark_operation_started(recorded->op_id, lease->epoch, now_)
                    .has_value());
    ASSERT_TRUE(store_
                    ->confirm_operation(recorded->op_id, R"({"number": 7})",
                                        lease->epoch, now_)
                    .has_value());

    transport_ = std::make_unique<ScriptedHttpTransport>();
  }

  void TearDown() override {
    transport_.reset();
    store_.reset();
    repository_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  cf::cli::CleanupOptions options() {
    cf::cli::CleanupOptions options;
    options.workspace = dir_;
    options.run_id = "R12";
    options.repository_path = dir_ / "repo";
    options.github_owner = "o";
    options.github_repo = "r";
    options.github_token = "ghs_test";
    options.github_transport = transport_.get();
    return options;
  }

  std::filesystem::path dir_;
  cf::store::UnixTime now_ = 0;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<cf::git::Repository> repository_;
  std::unique_ptr<cf::store::RunStore> store_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
};

TEST_F(CleanupTest, DeletesTheRunsBranchesAfterRetention) {
  transport_->script(200,
                     R"({"number": 7, "state": "closed", "merged_at": "2020-01-01T00:00:00Z"})");
  transport_->script(204, "");
  transport_->script(204, "");

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cf::cli::cleanup_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_EQ(document["reason"], "deleted");
  EXPECT_EQ(document["deleted"].size(), 2u);

  // The GitHub API (not the local remote) performs the deletion; verify the
  // exact DELETE calls and that an unrelated run's branch is untouched.
  bool deleted_milestone = false;
  bool deleted_integration = false;
  for (const auto& request : transport_->requests()) {
    if (request.method != "DELETE") {
      continue;
    }
    EXPECT_EQ(request.url.find("cf/R99/keep"), std::string::npos);
    deleted_milestone =
        deleted_milestone ||
        request.url.find("/git/refs/heads/cf/R12/M1") != std::string::npos;
    deleted_integration =
        deleted_integration ||
        request.url.find("/git/refs/heads/cf/R12/integration") !=
            std::string::npos;
  }
  EXPECT_TRUE(deleted_milestone);
  EXPECT_TRUE(deleted_integration);
}

TEST_F(CleanupTest, RetainsTheBranchesInsideTheWindow) {
  transport_->script(200,
                     R"({"number": 7, "state": "closed", "closed_at": ")" +
                         recent_iso_time(86400) + R"("})");
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cf::cli::cleanup_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_EQ(document["reason"], "retained");

  auto milestone = repository_->remote_branch_sha("origin", "cf/R12/M1");
  ASSERT_TRUE(milestone.has_value());
  EXPECT_FALSE(milestone->empty());
  for (const auto& request : transport_->requests()) {
    EXPECT_NE(request.method, "DELETE");
  }
}

TEST_F(CleanupTest, AllSweepsTheWorkspace) {
  transport_->script(200,
                     R"({"number": 7, "state": "closed", "merged_at": "2020-01-01T00:00:00Z"})");
  transport_->script(204, "");
  transport_->script(204, "");

  cf::cli::CleanupOptions all = options();
  all.all = true;
  all.run_id.clear();
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cf::cli::cleanup_command(all, out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  ASSERT_EQ(document["runs"].size(), 1u);
  EXPECT_EQ(document["runs"][0]["reason"], "deleted");
}

TEST_F(CleanupTest, LeavesAnOpenPullRequestAlone) {
  transport_->script(200, R"({"number": 7, "state": "open"})");
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cf::cli::cleanup_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_EQ(document["reason"], "pull_request_open");
  for (const auto& request : transport_->requests()) {
    EXPECT_NE(request.method, "DELETE");
  }
}

}  // namespace
