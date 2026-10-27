// Unit tests for the GitHub operation handlers (design sections 6.12, 10.4).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/orch/github_handlers.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::Client;
using cf::github::ClientOptions;
using cf::orch::DispatchOutcome;
using cf::orch::DispatchResult;
using cf::orch::GithubHandlerContext;
using cf::orch::CommentHandler;
using cf::orch::IssueCreateHandler;
using cf::orch::IssueUpdateHandler;
using cf::orch::PrCreateHandler;
using cf::orch::PrUpdateHandler;
using cf::orch::PushHandler;
using cf::store::OperationRecord;
using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_gh_handlers_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

OperationRecord push_op(const std::string& commit, const std::string& old) {
  OperationRecord op;
  op.op_id = "op_push_1";
  op.kind = "push";
  op.input_json = nlohmann::json{{"remote", "origin"},
                                 {"branch", "cf/R12/M3"},
                                 {"commit", commit}}
                      .dump();
  op.expected_json = nlohmann::json{{"old", old}, {"new", commit}}.dump();
  return op;
}

class GithubHandlersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));
    write_file(dir_ / "repo" / "src" / "a.cpp", "int a() { return 0; }\n");
    cf::git::CommitTrailers trailers;
    trailers.run_id = "R12";
    trailers.milestone_id = "M3";
    trailers.op_id = "op_c1";
    auto sha =
        repository_->commit_paths(dir_ / "repo", {"src/a.cpp"}, "feat: a", trailers);
    ASSERT_TRUE(sha.has_value()) << sha.error().message;
    commit_ = *sha;

    // A bare remote.
    cf::process::ProcessRequest init;
    init.argv = {"git", "init", "--bare", (dir_ / "remote.git").string()};
    init.timeout = std::chrono::seconds(30);
    const auto result = executor_.run(init);
    ASSERT_TRUE(result.launched && result.exit_code == 0) << result.stderr_text;
    ASSERT_TRUE(repository_
                    ->command({"remote", "add", "origin",
                               (dir_ / "remote.git").string()})
                    .has_value());

    transport_ = std::make_unique<ScriptedHttpTransport>();
    ClientOptions options;
    options.api_base = "https://api.github.test";
    options.token = "ghs_test";
    client_ = std::make_unique<Client>(options, *transport_);
  }

  void TearDown() override {
    client_.reset();
    transport_.reset();
    repository_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  GithubHandlerContext context() {
    GithubHandlerContext context;
    context.repository = repository_.get();
    context.client = client_.get();
    context.owner = "o";
    context.repo = "r";
    return context;
  }

  std::filesystem::path dir_;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<cf::git::Repository> repository_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
  std::unique_ptr<Client> client_;
  std::string commit_;
};

TEST_F(GithubHandlersTest, PushLandsOnTheRemoteAndIsIdempotent) {
  PushHandler handler(context());
  const OperationRecord op = push_op(commit_, "");

  const DispatchResult performed = handler.perform(op);
  EXPECT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  auto remote = repository_->remote_branch_sha("origin", "cf/R12/M3");
  ASSERT_TRUE(remote.has_value());
  EXPECT_EQ(*remote, commit_);

  // Reconciling after the effect confirms instead of pushing again.
  const DispatchResult reconciled = handler.reconcile(op);
  EXPECT_EQ(reconciled.outcome, DispatchOutcome::Confirmed);
}

TEST_F(GithubHandlersTest, PushReconcileRetriesWhenItNeverLanded) {
  PushHandler handler(context());
  const OperationRecord op = push_op(commit_, "");
  const DispatchResult reconciled = handler.reconcile(op);
  EXPECT_EQ(reconciled.outcome, DispatchOutcome::Retry);
}

TEST_F(GithubHandlersTest, PushNeedsReconcileWhenTheRemoteMoved) {
  // Someone else created the branch at a different commit.
  write_file(dir_ / "repo" / "src" / "b.cpp", "int b() { return 0; }\n");
  cf::git::CommitTrailers trailers;
  trailers.op_id = "op_other";
  auto other =
      repository_->commit_paths(dir_ / "repo", {"src/b.cpp"}, "feat: b", trailers);
  ASSERT_TRUE(other.has_value());
  ASSERT_TRUE(repository_
                  ->command({"push", "origin", *other + ":refs/heads/cf/R12/M3"})
                  .has_value());

  PushHandler handler(context());
  const DispatchResult result = handler.perform(push_op(commit_, ""));
  EXPECT_EQ(result.outcome, DispatchOutcome::NeedsReconcile);
  EXPECT_NE(result.detail.find("expected it to be absent"), std::string::npos);

  // With an expected old commit, a different remote head is also escalated.
  const DispatchResult with_old = handler.perform(push_op(commit_, "deadbeef"));
  EXPECT_EQ(with_old.outcome, DispatchOutcome::NeedsReconcile);
  EXPECT_NE(with_old.detail.find("never force-push"), std::string::npos);
}

TEST_F(GithubHandlersTest, PrCreateUsesTheOpMarkerAndDedupes) {
  transport_->script(200, "[]");  // dedupe search: no PR yet
  transport_->script(201,
                     R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                         "draft": true, "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  OperationRecord op;
  op.op_id = "op_pr_1";
  op.kind = "pr_create";
  op.input_json = nlohmann::json{{"base", "main"},
                                 {"head", "cf/R12/integration"},
                                 {"title", "CF R12"},
                                 {"body", "summary"},
                                 {"draft", true}}
                      .dump();

  PrCreateHandler handler(context());
  const DispatchResult performed = handler.perform(op);
  ASSERT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  EXPECT_NE(performed.result_json.find("\"number\":7"), std::string::npos);
  ASSERT_EQ(transport_->requests().size(), 2u);
  EXPECT_NE(transport_->requests()[1].body.find("<!-- cf-op: op_pr_1 -->"),
            std::string::npos);

  // A reconciler finds the existing PR by marker and does not create a second.
  transport_->script(200,
                     R"([{"number": 7, "html_url": "u", "state": "open",
                          "body": "summary\n\n<!-- cf-op: op_pr_1 -->", "draft": true,
                          "head": {"ref": "cf/R12/integration"},
                          "base": {"ref": "main"}}])");
  const DispatchResult reconciled = handler.reconcile(op);
  EXPECT_EQ(reconciled.outcome, DispatchOutcome::Confirmed);
  EXPECT_EQ(transport_->requests().size(), 3u);  // only the GET, no second POST
}

TEST_F(GithubHandlersTest, PrCreateAppliesLabels) {
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                         "draft": false, "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  transport_->script(200, R"([{"name": "cf-generated"}])");
  OperationRecord op;
  op.op_id = "op_pr_labels";
  op.kind = "pr_create";
  op.input_json =
      nlohmann::json{{"base", "main"},
                     {"head", "cf/R12/integration"},
                     {"title", "CF R12"},
                     {"body", "summary"},
                     {"draft", false},
                     {"labels", nlohmann::json::array({"cf-generated"})}}
          .dump();

  PrCreateHandler handler(context());
  const DispatchResult result = handler.perform(op);
  ASSERT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  ASSERT_EQ(transport_->requests().size(), 3u);
  EXPECT_EQ(transport_->requests()[2].method, "POST");
  EXPECT_NE(transport_->requests()[2].url.find("/repos/o/r/issues/7/labels"),
            std::string::npos);
  const auto payload = nlohmann::json::parse(transport_->requests()[2].body);
  EXPECT_EQ(payload.at("labels").at(0).get<std::string>(), "cf-generated");
}

TEST_F(GithubHandlersTest, PrCreateClientErrorsFail) {
  transport_->script(200, "[]");
  transport_->script(422, R"({"message": "Validation Failed"})");
  OperationRecord op;
  op.op_id = "op_pr_2";
  op.kind = "pr_create";
  op.input_json =
      nlohmann::json{{"base", "main"}, {"head", "cf/R12/integration"}, {"title", "t"}}
          .dump();
  PrCreateHandler handler(context());
  const DispatchResult result = handler.perform(op);
  EXPECT_EQ(result.outcome, DispatchOutcome::Failed);
}

TEST_F(GithubHandlersTest, IssueCreateUsesTheOpMarkerAndDedupes) {
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 3, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");
  OperationRecord op;
  op.op_id = "op_issue_1";
  op.kind = "issue_create";
  op.input_json = nlohmann::json{{"title", "Duplicate ids"},
                                 {"body", "details"},
                                 {"labels", nlohmann::json::array({"cf-defect"})}}
                      .dump();

  IssueCreateHandler handler(context());
  const DispatchResult performed = handler.perform(op);
  ASSERT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  EXPECT_NE(performed.result_json.find("\"number\":3"), std::string::npos);
  EXPECT_NE(transport_->requests()[1].body.find("<!-- cf-op: op_issue_1 -->"),
            std::string::npos);

  transport_->script(200,
                     R"([{"number": 3, "html_url": "u", "title": "t",
                          "body": "details\n\n<!-- cf-op: op_issue_1 -->",
                          "state": "open"}])");
  const DispatchResult reconciled = handler.reconcile(op);
  EXPECT_EQ(reconciled.outcome, DispatchOutcome::Confirmed);
  EXPECT_EQ(transport_->requests().size(), 3u);
}

TEST_F(GithubHandlersTest, PrUpdateStampsTheMarkerAndReconciles) {
  transport_->script(200, R"({"number": 7, "html_url": "u", "state": "open",
                               "body": "new body", "draft": false,
                               "head": {"ref": "cf/R12/integration"},
                               "base": {"ref": "main"}})");
  // Marking ready looks up CODEOWNERS (.github/, root, docs/); none here.
  transport_->script(404, R"({"message": "Not Found"})");
  transport_->script(404, R"({"message": "Not Found"})");
  transport_->script(404, R"({"message": "Not Found"})");
  OperationRecord op;
  op.op_id = "op_pr_update_1";
  op.kind = "pr_update";
  op.input_json = nlohmann::json{{"number", 7},
                                 {"body", "milestones: M3 done"},
                                 {"mark_ready", true}}
                      .dump();
  op.expected_json = "{}";

  PrUpdateHandler handler(context());
  const DispatchResult performed = handler.perform(op);
  ASSERT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  const auto& request = transport_->requests().front();
  EXPECT_EQ(request.method, "PATCH");
  EXPECT_NE(request.body.find("\"draft\":false"), std::string::npos);
  EXPECT_NE(request.body.find("<!-- cf-op: op_pr_update_1 -->"), std::string::npos);

  transport_->script(200,
                     R"({"number": 7, "html_url": "u", "state": "open",
                         "body": "milestones: M3 done\n\n<!-- cf-op: op_pr_update_1 -->",
                         "draft": false, "head": {"ref": "cf/R12/integration"},
                         "base": {"ref": "main"}})");
  transport_->script(404, R"({"message": "Not Found"})");
  transport_->script(404, R"({"message": "Not Found"})");
  transport_->script(404, R"({"message": "Not Found"})");
  const DispatchResult reconciled = handler.reconcile(op);
  EXPECT_EQ(reconciled.outcome, DispatchOutcome::Confirmed);

  // Without the marker the reconciler asks for a retry.
  transport_->script(200, R"({"number": 7, "html_url": "u", "state": "open",
                               "body": "something else", "draft": false,
                               "head": {"ref": "cf/R12/integration"},
                               "base": {"ref": "main"}})");
  EXPECT_EQ(handler.reconcile(op).outcome, DispatchOutcome::Retry);
}

TEST_F(GithubHandlersTest, PrUpdateCanCloseThePullRequest) {
  transport_->script(200, R"({"number": 7, "html_url": "u", "state": "closed",
                               "body": "closed body", "draft": false,
                               "head": {"ref": "cf/R12/integration"},
                               "base": {"ref": "main"}})");
  OperationRecord op;
  op.op_id = "op_pr_close";
  op.kind = "pr_update";
  op.input_json = nlohmann::json{{"number", 7},
                                 {"body", "run aborted"},
                                 {"mark_ready", false},
                                 {"state", "closed"}}
                      .dump();
  op.expected_json = "{}";
  PrUpdateHandler handler(context());
  const DispatchResult performed = handler.perform(op);
  ASSERT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  EXPECT_NE(transport_->requests().front().body.find("\"state\":\"closed\""),
            std::string::npos);
}

TEST_F(GithubHandlersTest, IssueCreateDedupesByFingerprintAcrossRuns) {
  // An earlier run already filed the same defect: the issue carries the
  // fingerprint marker, so no second issue is opened.
  transport_->script(200,
                     R"([{"number": 9, "html_url": "u", "title": "t",
                          "body": "old issue\n\n<!-- cf-fingerprint: sha256:abc -->",
                          "state": "open"}])");
  OperationRecord op;
  op.op_id = "op_issue_2";
  op.kind = "issue_create";
  op.input_json = nlohmann::json{{"title", "t"},
                                 {"body", "b"},
                                 {"labels", nlohmann::json::array({"cf-defect"})},
                                 {"defect_id", "D-1"},
                                 {"fingerprint", "sha256:abc"}}
                      .dump();
  IssueCreateHandler handler(context());
  const DispatchResult result = handler.perform(op);
  ASSERT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  EXPECT_NE(result.result_json.find("\"number\":9"), std::string::npos);
  EXPECT_EQ(transport_->requests().size(), 1u);  // the GET only, no POST
}

TEST_F(GithubHandlersTest, IssueUpdateSyncsTheStatusAndReconciles) {
  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "closed", "state": "closed"})");
  OperationRecord op;
  op.op_id = "op_issue_update_1";
  op.kind = "issue_update";
  op.input_json = nlohmann::json{{"number", 5},
                                 {"body", "status: VERIFIED"},
                                 {"state", "closed"},
                                 {"defect_id", "D-1"}}
                      .dump();
  op.expected_json = "{}";
  IssueUpdateHandler handler(context());
  const DispatchResult performed = handler.perform(op);
  ASSERT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  const auto& request = transport_->requests().front();
  EXPECT_EQ(request.method, "PATCH");
  EXPECT_NE(request.body.find("\"state\":\"closed\""), std::string::npos);
  EXPECT_NE(request.body.find("<!-- cf-op: op_issue_update_1 -->"), std::string::npos);

  transport_->script(200,
                     R"({"number": 5, "html_url": "u", "title": "t",
                         "body": "x\n\n<!-- cf-op: op_issue_update_1 -->",
                         "state": "closed"})");
  EXPECT_EQ(handler.reconcile(op).outcome, DispatchOutcome::Confirmed);

  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "x", "state": "open"})");
  EXPECT_EQ(handler.reconcile(op).outcome, DispatchOutcome::Retry);
}

TEST_F(GithubHandlersTest, CommentStampsTheMarkerAndReconciles) {
  transport_->script(201, R"({"id": 99})");
  OperationRecord op;
  op.op_id = "op_comment_1";
  op.kind = "comment";
  op.input_json =
      nlohmann::json{{"issue_number", 7}, {"body", "escalation bundle: ..."}}.dump();
  op.expected_json = "{}";

  CommentHandler handler(context());
  const DispatchResult performed = handler.perform(op);
  ASSERT_EQ(performed.outcome, DispatchOutcome::Confirmed) << performed.detail;
  EXPECT_NE(transport_->requests().front().body.find("<!-- cf-op: op_comment_1 -->"),
            std::string::npos);

  transport_->script(200, R"([{"body": "old"}, {"body": "x\n<!-- cf-op: op_comment_1 -->"}])");
  EXPECT_EQ(handler.reconcile(op).outcome, DispatchOutcome::Confirmed);

  transport_->script(200, R"([{"body": "old"}])");
  EXPECT_EQ(handler.reconcile(op).outcome, DispatchOutcome::Retry);
}

}  // namespace
