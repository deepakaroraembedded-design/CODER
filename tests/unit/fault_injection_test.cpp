// Fault-injection tests for the side-effect protocol (design section
// 6.12.7): a crash after the intent, a crash between the effect and the
// confirm, the stale compare-and-swap push, and the SIGSTOP takeover with a
// request in flight (design section 6.12.6 rule 4).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/common/hash.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/orch/github_handlers.hpp"
#include "cf/orch/recovery.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::Client;
using cf::github::ClientOptions;
using cf::orch::CommentHandler;
using cf::orch::Dispatcher;
using cf::orch::DispatchOutcome;
using cf::orch::GithubHandlerContext;
using cf::orch::IssueCreateHandler;
using cf::orch::IssueUpdateHandler;
using cf::orch::PrCreateHandler;
using cf::orch::PrUpdateHandler;
using cf::orch::PushHandler;
using cf::orch::Recovery;
using cf::orch::RecoveryOptions;
using cf::store::NewOperation;
using cf::store::OperationRecord;
using cf::store::RunRecord;
using cf::store::RunStore;
using cf::store::UnixTime;
using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_fault_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

// A miniature stateful issue-comment endpoint: POST appends, GET lists,
// DELETE erases. The first POST can be held mid-flight until released —
// the stalled orchestrator's in-flight request in the SIGSTOP scenario
// (design section 6.12.7).
class FakeCommentEndpoint final : public cf::llm::HttpTransport {
 public:
  void hold_first_post() {
    std::lock_guard<std::mutex> lock(mutex_);
    hold_post_ = true;
  }

  // Blocks until the held POST is in flight.
  void wait_until_held() {
    std::unique_lock<std::mutex> lock(mutex_);
    held_cv_.wait(lock, [&] { return held_; });
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

  cf::Result<cf::llm::HttpResponse> post(const cf::llm::HttpRequest& request) override {
    bool mine = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (hold_post_ && !held_) {
        held_ = true;
        mine = true;
        held_cv_.notify_all();
      }
    }
    if (mine) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return released_; });
    }
    const auto body = nlohmann::json::parse(request.body, nullptr, false);
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t id = next_id_++;
    comments_.emplace_back(id, body.is_discarded() ? "" : body.value("body", ""));
    cf::llm::HttpResponse response;
    response.status = 201;
    response.body = nlohmann::json{{"id", id}}.dump();
    return response;
  }

  cf::Result<cf::llm::HttpResponse> get(const cf::llm::HttpRequest& request) override {
    (void)request;
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json array = nlohmann::json::array();
    for (const auto& [id, body] : comments_) {
      array.push_back({{"id", id}, {"body", body}});
    }
    cf::llm::HttpResponse response;
    response.status = 200;
    response.body = array.dump();
    return response;
  }

  cf::Result<cf::llm::HttpResponse> del(const cf::llm::HttpRequest& request) override {
    const auto slash = request.url.find_last_of('/');
    const int64_t id = std::stoll(request.url.substr(slash + 1));
    std::lock_guard<std::mutex> lock(mutex_);
    comments_.erase(std::remove_if(comments_.begin(), comments_.end(),
                                   [id](const auto& comment) {
                                     return comment.first == id;
                                   }),
                    comments_.end());
    cf::llm::HttpResponse response;
    response.status = 204;
    return response;
  }

  std::vector<std::pair<int64_t, std::string>> comments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return comments_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable held_cv_;
  bool hold_post_ = false;
  bool held_ = false;
  bool released_ = false;
  int64_t next_id_ = 1;
  std::vector<std::pair<int64_t, std::string>> comments_;
};

class FaultInjectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    now_ = cf::store::unix_now();

    RunRecord run;
    run.run_id = "R12";
    run.state = "Executing";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());

    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));
    write_file(dir_ / "repo" / "README.md", "initial\n");
    cf::git::CommitTrailers trailers;
    trailers.run_id = "R12";
    trailers.op_id = "op_init";
    auto c0 = repository_->commit_paths(dir_ / "repo", {"README.md"}, "chore: init",
                                        trailers);
    ASSERT_TRUE(c0.has_value()) << c0.error().message;
    commit0_ = *c0;
    write_file(dir_ / "repo" / "src" / "a.cpp", "int a() { return 0; }\n");
    trailers.op_id = "op_c1";
    auto c1 = repository_->commit_paths(dir_ / "repo", {"src/a.cpp"}, "feat: a",
                                        trailers);
    ASSERT_TRUE(c1.has_value()) << c1.error().message;
    commit1_ = *c1;

    cf::process::ProcessRequest init;
    init.argv = {"git", "init", "--bare", (dir_ / "remote.git").string()};
    init.timeout = std::chrono::seconds(30);
    const auto bare = executor_.run(init);
    ASSERT_TRUE(bare.launched && bare.exit_code == 0) << bare.stderr_text;
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
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  GithubHandlerContext context() {
    GithubHandlerContext context;
    context.repository = repository_.get();
    context.client = client_.get();
    context.owner = "o";
    context.repo = "r";
    context.remote = "origin";
    return context;
  }

  // The six GitHub operation kinds, registered on one dispatcher (the
  // production wiring in RunProcess, design section 6.12.4).
  void register_handlers(Dispatcher& dispatcher) {
    dispatcher.register_handler(std::make_unique<PushHandler>(context()));
    dispatcher.register_handler(std::make_unique<PrCreateHandler>(context()));
    dispatcher.register_handler(std::make_unique<PrUpdateHandler>(context()));
    dispatcher.register_handler(std::make_unique<IssueCreateHandler>(context()));
    dispatcher.register_handler(std::make_unique<IssueUpdateHandler>(context()));
    dispatcher.register_handler(std::make_unique<CommentHandler>(context()));
  }

  std::string seed_intent(const std::string& kind, const nlohmann::json& input,
                          const nlohmann::json& expected = nlohmann::json::object(),
                          int64_t sequence = 0) {
    NewOperation intent;
    intent.idempotency_key =
        cf::idempotency_key("R12", "M3", kind, cf::sha256_hex(input.dump()));
    intent.milestone_id = "M3";
    intent.kind = kind;
    intent.input_json = input.dump();
    intent.expected_json = expected.dump();
    // created_at orders the pending list; keep the intents in seed order so
    // the scripted transport's FIFO responses line up with the requests.
    const auto recorded =
        store_->record_intents("R12", {intent}, now_ - 1000 + sequence);
    EXPECT_TRUE(recorded.has_value()) << recorded.error().message;
    auto op = store_->find_operation_by_key(intent.idempotency_key);
    EXPECT_TRUE(op.has_value());
    return op->op_id;
  }

  std::vector<std::string> seed_all_six() {
    int64_t sequence = 0;
    return {
        seed_intent("push",
                    {{"remote", "origin"}, {"branch", "cf/R12/M3"}, {"commit", commit1_}},
                    {{"old", ""}, {"new", commit1_}}, sequence++),
        seed_intent("pr_create",
                    {{"head", "cf/R12/M3"}, {"base", "main"}, {"title", "M3"}},
                    nlohmann::json::object(), sequence++),
        seed_intent("pr_update", {{"number", 3}, {"body", "updated"}},
                    nlohmann::json::object(), sequence++),
        seed_intent("issue_create", {{"title", "defect"}, {"body", "details"}},
                    nlohmann::json::object(), sequence++),
        seed_intent("issue_update", {{"number", 9}, {"body", "sync"}, {"state", ""}},
                    nlohmann::json::object(), sequence++),
        seed_intent("comment", {{"issue_number", 9}, {"body", "note"}},
                    nlohmann::json::object(), sequence++),
    };
  }

  // The old orchestrator held the lease and died; recovery takes over at a
  // new epoch (design section 6.12.4).
  cf::Result<cf::orch::RecoveryReport> recover(Dispatcher& dispatcher) {
    Recovery recovery(*store_);
    RecoveryOptions options;
    options.holder = "new-orch";
    options.in_flight_grace = std::chrono::seconds(0);
    return recovery.resume("R12", dispatcher, options, now_);
  }

  std::filesystem::path dir_;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<RunStore> store_;
  std::unique_ptr<cf::git::Repository> repository_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
  std::unique_ptr<Client> client_;
  std::string commit0_;
  std::string commit1_;
  UnixTime now_ = 0;
};

// A crash after the intents were recorded but before anything was
// dispatched: recovery re-dispatches and every effect lands exactly once.
TEST_F(FaultInjectionTest, ACrashAfterTheIntentLandsEveryEffectExactlyOnce) {
  const std::vector<std::string> op_ids = seed_all_six();
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now_ - 1000)
                  .has_value());
  // The crash: nothing was ever dispatched.

  // The new orchestrator's dispatch scripts (in intent order; push is git).
  transport_->script(200, "[]");  // pr_create: no existing PR
  transport_->script(201, R"({"number": 3, "html_url": "u3", "state": "open",
                              "body": "b", "draft": true,
                              "head": {"ref": "cf/R12/M3"}, "base": {"ref": "main"}})");
  transport_->script(200, R"({"number": 3, "html_url": "u3", "state": "open",
                              "body": "b", "draft": true,
                              "head": {"ref": "cf/R12/M3"}, "base": {"ref": "main"}})");
  transport_->script(200, "[]");  // issue_create: no existing issue
  transport_->script(201, R"({"number": 9, "html_url": "u9", "state": "open",
                              "body": "b"})");
  transport_->script(200, R"({"number": 9, "html_url": "u9", "state": "open",
                              "body": "b"})");
  transport_->script(201, R"({"id": 501})");  // the comment

  Dispatcher dispatcher(*store_, "R12", "new-orch");
  register_handlers(dispatcher);
  auto report = recover(dispatcher);
  ASSERT_TRUE(report.has_value()) << report.error().message;

  auto dispatched = dispatcher.dispatch_pending(report->epoch, now_);
  ASSERT_TRUE(dispatched.has_value()) << dispatched.error().message;
  EXPECT_EQ(*dispatched, 6u);

  for (const auto& op_id : op_ids) {
    auto op = store_->get_operation(op_id);
    ASSERT_TRUE(op.has_value()) << op.error().message;
    EXPECT_EQ(op->state, cf::store::op_state::kConfirmed) << op->kind;
  }
  // The push landed on the remote.
  auto remote = repository_->remote_branch_sha("origin", "cf/R12/M3");
  ASSERT_TRUE(remote.has_value()) << remote.error().message;
  EXPECT_EQ(*remote, commit1_);

  // Exactly one mutating request per REST kind.
  std::size_t pr_posts = 0, pr_patches = 0, issue_posts = 0, issue_patches = 0,
              comment_posts = 0;
  for (const auto& request : transport_->requests()) {
    if (request.method == "POST" && request.url.find("/pulls") != std::string::npos)
      ++pr_posts;
    if (request.method == "PATCH" && request.url.find("/pulls/3") != std::string::npos)
      ++pr_patches;
    if (request.method == "POST" &&
        request.url.find("/issues") != std::string::npos &&
        request.url.find("comments") == std::string::npos)
      ++issue_posts;
    if (request.method == "PATCH" && request.url.find("/issues/9") != std::string::npos)
      ++issue_patches;
    if (request.method == "POST" && request.url.find("/comments") != std::string::npos)
      ++comment_posts;
  }
  EXPECT_EQ(pr_posts, 1u);
  EXPECT_EQ(pr_patches, 1u);
  EXPECT_EQ(issue_posts, 1u);
  EXPECT_EQ(issue_patches, 1u);
  EXPECT_EQ(comment_posts, 1u);

  // Idempotent: a second cycle dispatches nothing and issues no request.
  auto again = dispatcher.dispatch_pending(report->epoch, now_);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, 0u);
  const std::size_t requests_so_far = transport_->requests().size();
  auto second = recover(dispatcher);
  ASSERT_TRUE(second.has_value()) << second.error().message;
  auto redispatched = dispatcher.dispatch_pending(second->epoch, now_);
  ASSERT_TRUE(redispatched.has_value());
  EXPECT_EQ(*redispatched, 0u);
  EXPECT_EQ(transport_->requests().size(), requests_so_far);
}

// A crash between the effect and the confirm: reconciliation finds the
// landed effect and confirms it without ever re-performing it.
TEST_F(FaultInjectionTest, ACrashBetweenEffectAndConfirmNeverDuplicates) {
  const std::vector<std::string> op_ids = seed_all_six();
  // The old orchestrator started every operation under epoch 1 and died
  // before confirming.
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now_ - 1000)
                  .has_value());
  for (const auto& op_id : op_ids) {
    ASSERT_TRUE(store_->mark_operation_started(op_id, 1, now_ - 1000).has_value());
  }

  // But every effect had in fact landed before the crash:
  ASSERT_TRUE(  // the push
      repository_->command({"push", "origin", commit1_ + ":refs/heads/cf/R12/M3"})
          .has_value());
  const std::string marker_pr = "<!-- cf-op: " + op_ids[1] + " -->";
  const std::string marker_pr_update = "<!-- cf-op: " + op_ids[2] + " -->";
  const std::string marker_issue = "<!-- cf-op: " + op_ids[3] + " -->";
  const std::string marker_issue_update = "<!-- cf-op: " + op_ids[4] + " -->";
  const std::string marker_comment = "<!-- cf-op: " + op_ids[5] + " -->";
  transport_->script(200, /* existing PR with the marker */
                     R"([{"number": 3, "html_url": "u3", "state": "open",
                          "body": "b\n)" + marker_pr +
                         R"(", "draft": true, "head": {"ref": "cf/R12/M3"},
                          "base": {"ref": "main"}}])");
  transport_->script(200, /* the PR carries the pr_update marker */
                     R"({"number": 3, "html_url": "u3", "state": "open",
                          "body": "b\n)" + marker_pr_update +
                         R"(", "draft": true, "head": {"ref": "cf/R12/M3"},
                          "base": {"ref": "main"}})");
  transport_->script(200, /* existing issue with the marker */
                     R"([{"number": 9, "html_url": "u9", "state": "open",
                          "body": "b\n)" + marker_issue + R"("}])");
  transport_->script(200, /* the issue carries the issue_update marker */
                     R"({"number": 9, "html_url": "u9", "state": "open",
                          "body": "b\n)" + marker_issue_update + R"("})");
  transport_->script(200, /* the comment with the marker */
                     R"([{"id": 501, "body": "b\n)" + marker_comment + R"("}])");

  Dispatcher dispatcher(*store_, "R12", "new-orch");
  register_handlers(dispatcher);
  auto report = recover(dispatcher);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->started_reconciled, 6u);

  for (const auto& op_id : op_ids) {
    auto op = store_->get_operation(op_id);
    ASSERT_TRUE(op.has_value()) << op.error().message;
    EXPECT_EQ(op->state, cf::store::op_state::kConfirmed) << op->kind;
  }

  // Not a single mutating request was issued: reconciliation is read-only.
  for (const auto& request : transport_->requests()) {
    EXPECT_EQ(request.method, "GET") << request.url;
  }
  // And the remote branch moved only by the pre-landed push.
  auto remote = repository_->remote_branch_sha("origin", "cf/R12/M3");
  ASSERT_TRUE(remote.has_value());
  EXPECT_EQ(*remote, commit1_);
}

// The git-level fencing primitive (design section 6.12.6 rule 4): a push
// made compare-and-swap fails when the branch is no longer at the expected
// commit, so a late request from a stalled orchestrator never overwrites.
TEST_F(FaultInjectionTest, AStaleCompareAndSwapPushFailsAndLeavesTheBranch) {
  // The branch moved past the stalled orchestrator's expectation.
  ASSERT_TRUE(
      repository_->command({"push", "origin", commit1_ + ":refs/heads/cf/R12/M3"})
          .has_value());

  // The late attempt, expecting the branch to be absent or at commit0_.
  const auto stale = repository_->command(
      {"push", "--force-with-lease=refs/heads/cf/R12/M3:" + commit0_, "origin",
       commit0_ + ":refs/heads/cf/R12/M3"});
  EXPECT_FALSE(stale.has_value());
  auto remote = repository_->remote_branch_sha("origin", "cf/R12/M3");
  ASSERT_TRUE(remote.has_value());
  EXPECT_EQ(*remote, commit1_);  // unmoved

  // Control: the same push with the correct expectation succeeds.
  const auto current = repository_->command(
      {"push", "--force-with-lease=refs/heads/cf/R12/M3:" + commit1_, "origin",
       commit1_ + ":refs/heads/cf/R12/M3"});
  EXPECT_TRUE(current.has_value()) << current.error().message;
}

// The SIGSTOP scenario (design section 6.12.7): an orchestrator is paused
// with a comment request in flight, a second orchestrator takes over, then
// the first resumes and its request lands late.
TEST_F(FaultInjectionTest, SigstopTakeoverWithAnInFlightCommentIsFenced) {
  const UnixTime past = now_ - 1000;
  ASSERT_TRUE(
      store_->acquire_lease("R12", "old-orch", std::chrono::seconds(30), past)
          .has_value());
  const std::string op_id =
      seed_intent("comment", {{"issue_number", 7}, {"body", "escalation"}});

  FakeCommentEndpoint endpoint;
  endpoint.hold_first_post();
  ClientOptions options;
  options.api_base = "https://api.github.test";
  options.token = "ghs_test";
  Client client_a(options, endpoint);
  Client client_b(options, endpoint);
  auto context_for = [&](Client& client) {
    GithubHandlerContext context;
    context.repository = repository_.get();
    context.client = &client;
    context.owner = "o";
    context.repo = "r";
    return context;
  };

  Dispatcher dispatcher_a(*store_, "R12", "old-orch");
  dispatcher_a.register_handler(std::make_unique<CommentHandler>(context_for(client_a)));

  // The old orchestrator dispatches; the POST blocks mid-flight.
  std::optional<cf::Result<std::size_t>> result_a;
  std::thread stalled(
      [&] { result_a.emplace(dispatcher_a.dispatch_pending(1, past)); });
  endpoint.wait_until_held();

  // The lease expired; the new orchestrator takes over, reconciles (nothing
  // has landed), re-issues the comment and confirms it under epoch 2.
  Dispatcher dispatcher_b(*store_, "R12", "new-orch");
  dispatcher_b.register_handler(std::make_unique<CommentHandler>(context_for(client_b)));
  auto report = recover(dispatcher_b);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  ASSERT_EQ(report->epoch, 2u);
  auto dispatched = dispatcher_b.dispatch_pending(report->epoch, now_);
  ASSERT_TRUE(dispatched.has_value()) << dispatched.error().message;
  EXPECT_EQ(*dispatched, 1u);

  // The old orchestrator resumes: its request lands late; its epoch-1
  // confirm is fenced by the store (design section 6.12.6 rule 4).
  endpoint.release();
  stalled.join();
  ASSERT_TRUE(result_a.has_value());
  ASSERT_FALSE(result_a->has_value());
  EXPECT_EQ(result_a->error().code, cf::store::error_codes::kStaleEpoch);

  // Exactly one operation, confirmed under the new epoch; the fenced
  // duplicate landed on the server but was never confirmed.
  auto op = store_->get_operation(op_id);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kConfirmed);
  EXPECT_EQ(op->epoch, 2u);
  EXPECT_EQ(endpoint.comments().size(), 2u);
  // (The reconciler cleaning the duplicate when it finds two at reconcile
  // time is covered by ReconcileDeletesDuplicateCommentsAndKeepsTheEarliest.)
}

// The rule-4 cleanup itself: a reconcile that finds the same comment twice
// keeps the earliest and deletes the rest.
TEST_F(FaultInjectionTest, ReconcileDeletesDuplicateCommentsAndKeepsTheEarliest) {
  const std::string op_id =
      seed_intent("comment", {{"issue_number", 7}, {"body", "escalation"}});
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now_ - 1000)
                  .has_value());
  ASSERT_TRUE(store_->mark_operation_started(op_id, 1, now_ - 1000).has_value());

  const std::string marker = "<!-- cf-op: " + op_id + " -->";
  transport_->script(200, R"([{"id": 501, "body": "x\n)" + marker + R"("},
                              {"id": 502, "body": "x\n)" + marker + R"("}])");
  transport_->script(204, "");  // the duplicate's DELETE

  Dispatcher dispatcher(*store_, "R12", "new-orch");
  register_handlers(dispatcher);
  auto report = recover(dispatcher);
  ASSERT_TRUE(report.has_value()) << report.error().message;

  auto op = store_->get_operation(op_id);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kConfirmed);

  bool saw_delete = false;
  for (const auto& request : transport_->requests()) {
    if (request.method == "DELETE") {
      saw_delete = true;
      EXPECT_NE(request.url.find("/issues/comments/502"), std::string::npos);
    }
  }
  EXPECT_TRUE(saw_delete);
}

}  // namespace
