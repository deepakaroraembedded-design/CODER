// Unit tests for the GitHub flow (design section 10.4.4): milestone pushes,
// the draft PR lifecycle, escalation branches and abort handling.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/orch/github_flow.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::Client;
using cf::github::ClientOptions;
using cf::orch::Dispatcher;
using cf::orch::GithubFlow;
using cf::orch::GithubFlowConfig;
using cf::orch::GithubHandlerContext;
using cf::orch::IssueCreateHandler;
using cf::orch::PrCreateHandler;
using cf::orch::PrUpdateHandler;
using cf::orch::PushHandler;
using cf::orch::CommentHandler;
using cf::orch::RunEvent;
using cf::orch::RunState;
using cf::orch::StateMachine;
using cf::store::RunStore;
using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_github_flow_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

class GithubFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));
    write_file(dir_ / "repo" / "README.md", "initial\n");
    cf::git::CommitTrailers init;
    init.run_id = "R12";
    init.op_id = "op_init";
    ASSERT_TRUE(repository_
                    ->commit_paths(dir_ / "repo", {"README.md"}, "chore: init", init)
                    .has_value());

    cf::process::ProcessRequest bare;
    bare.argv = {"git", "init", "--bare", (dir_ / "remote.git").string()};
    bare.timeout = std::chrono::seconds(30);
    const auto result = executor_.run(bare);
    ASSERT_TRUE(result.launched && result.exit_code == 0) << result.stderr_text;
    ASSERT_TRUE(repository_
                    ->command({"remote", "add", "origin", (dir_ / "remote.git").string()})
                    .has_value());
    // Milestone and integration branches exist at the initial commit.
    ASSERT_TRUE(repository_
                    ->command({"branch", "cf-local/R12/M1/coder", "main"})
                    .has_value());
    ASSERT_TRUE(repository_
                    ->command({"branch", "cf/R12/integration", "main"})
                    .has_value());

    auto opened = RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value());
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Executing";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());
    cf::store::MilestoneRecord milestone;
    milestone.run_id = "R12";
    milestone.milestone_id = "M1";
    milestone.state = "Done";
    ASSERT_TRUE(store_->upsert_milestone(milestone).has_value());

    plan_.plan_id = "PLAN-X";
    plan_.product = "Y";
    plan_.version = "1";
    cf::plan::MilestoneSpec spec;
    spec.id = "M1";
    spec.title = "Core";
    plan_.milestones.push_back(spec);

    auto lease = store_->acquire_lease("R12", "test", std::chrono::seconds(60), now_);
    ASSERT_TRUE(lease.has_value());
    epoch_ = lease->epoch;

    transport_ = std::make_unique<ScriptedHttpTransport>();
    ClientOptions options;
    options.api_base = "https://api.github.test";
    options.token = "ghs_test";
    client_ = std::make_unique<Client>(options, *transport_);

    GithubHandlerContext context;
    context.repository = repository_.get();
    context.client = client_.get();
    context.owner = "o";
    context.repo = "r";
    dispatcher_ = std::make_unique<Dispatcher>(*store_, "R12", "test");
    dispatcher_->register_handler(std::make_unique<PushHandler>(context));
    dispatcher_->register_handler(std::make_unique<PrCreateHandler>(context));
    dispatcher_->register_handler(std::make_unique<PrUpdateHandler>(context));
    dispatcher_->register_handler(std::make_unique<IssueCreateHandler>(context));
    dispatcher_->register_handler(std::make_unique<CommentHandler>(context));
    GithubFlowConfig flow_config;
    flow_config.owner = "o";
    flow_config.repo = "r";
    flow_ = std::make_unique<GithubFlow>(*store_, *dispatcher_, *repository_,
                                         std::move(flow_config));
  }

  void TearDown() override {
    flow_.reset();
    dispatcher_.reset();
    client_.reset();
    transport_.reset();
    store_.reset();
    repository_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<cf::git::Repository> repository_;
  std::unique_ptr<RunStore> store_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
  std::unique_ptr<Client> client_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::unique_ptr<GithubFlow> flow_;
  cf::plan::Plan plan_;
  cf::store::UnixTime now_ = cf::store::unix_now();
  uint64_t epoch_ = 0;
};

TEST_F(GithubFlowTest, PushesMilestonesAndOpensADraftPullRequest) {
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                         "draft": true, "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");

  auto synced = flow_->sync("R12", plan_, RunState::Executing, epoch_, now_);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;
  EXPECT_EQ(*synced, 3u);  // two pushes + pr_create

  auto milestone_remote = repository_->remote_branch_sha("origin", "cf/R12/M1");
  ASSERT_TRUE(milestone_remote.has_value());
  EXPECT_FALSE(milestone_remote->empty());
  auto integration_remote =
      repository_->remote_branch_sha("origin", "cf/R12/integration");
  ASSERT_TRUE(integration_remote.has_value());
  EXPECT_FALSE(integration_remote->empty());

  ASSERT_EQ(transport_->requests().size(), 2u);
  EXPECT_EQ(transport_->requests()[1].method, "POST");
  EXPECT_NE(transport_->requests()[1].body.find("\"draft\":true"), std::string::npos);

  // A second sync does not repeat anything.
  const auto again = flow_->sync("R12", plan_, RunState::Executing, epoch_, now_);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, 0u);
  EXPECT_EQ(transport_->requests().size(), 2u);
}

TEST_F(GithubFlowTest, EscalationPushesABranchAndCommentsOnThePullRequest) {
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                         "draft": true, "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  auto synced = flow_->sync("R12", plan_, RunState::Executing, epoch_, now_);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;

  // The run escalates.
  StateMachine machine(*store_, "R12");
  const auto escalated = machine.apply(RunEvent::NoProgress, {}, now_ + 5);
  ASSERT_TRUE(escalated.has_value()) << escalated.error().message;
  ASSERT_EQ(*escalated, RunState::Escalated);

  transport_->script(200,
                     R"({"number": 7, "html_url": "u", "state": "open",
                         "body": "escalated", "draft": true,
                         "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  transport_->script(201, R"({"id": 99})");
  synced = flow_->sync("R12", plan_, RunState::Escalated, epoch_, now_ + 5);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;

  auto escalation_remote = repository_->remote_branch_sha(
      "origin", "cf/R12/escalation/esc-" + std::to_string(now_ + 5));
  ASSERT_TRUE(escalation_remote.has_value()) << escalation_remote.error().message;
  EXPECT_FALSE(escalation_remote->empty());

  bool commented = false;
  for (const auto& request : transport_->requests()) {
    if (request.method == "POST" && request.url.find("/issues/7/comments") != std::string::npos) {
      commented = request.body.find("bundle.json") != std::string::npos;
    }
  }
  EXPECT_TRUE(commented);
}

TEST_F(GithubFlowTest, AbortClosesTheDraftPullRequest) {
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                         "draft": true, "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  auto synced = flow_->sync("R12", plan_, RunState::Executing, epoch_, now_);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;

  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.apply(RunEvent::NoProgress, {}, now_ + 5).has_value());
  ASSERT_TRUE(machine.apply(RunEvent::HumanAborted, {}, now_ + 6).has_value());

  transport_->script(200,
                     R"({"number": 7, "html_url": "u", "state": "closed",
                         "body": "closed", "draft": false,
                         "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  synced = flow_->sync("R12", plan_, RunState::Aborted, epoch_, now_ + 6);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;

  bool closed = false;
  for (const auto& request : transport_->requests()) {
    if (request.method == "PATCH" &&
        request.body.find("\"state\":\"closed\"") != std::string::npos) {
      closed = true;
    }
  }
  EXPECT_TRUE(closed);
}

}  // namespace
