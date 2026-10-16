// Unit tests for the side-effect dispatcher (design section 6.12.2).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/common/hash.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::DispatchOutcome;
using cf::orch::Dispatcher;
using cf::orch::DispatchResult;
using cf::orch::OperationHandler;
using cf::store::NewOperation;
using cf::store::OperationRecord;
using cf::store::RunRecord;
using cf::store::RunStore;
using cf::store::UnixTime;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_dispatch_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class FakeHandler final : public OperationHandler {
 public:
  FakeHandler(std::string kind, DispatchResult perform_result,
              DispatchResult reconcile_result)
      : kind_(std::move(kind)),
        perform_result_(std::move(perform_result)),
        reconcile_result_(std::move(reconcile_result)) {}

  std::string_view kind() const override { return kind_; }

  DispatchResult perform(const OperationRecord& op) override {
    ++perform_calls;
    last_op = op;
    return perform_result_;
  }

  DispatchResult reconcile(const OperationRecord& op) override {
    ++reconcile_calls;
    last_op = op;
    return reconcile_result_;
  }

  int perform_calls = 0;
  int reconcile_calls = 0;
  OperationRecord last_op;

 private:
  std::string kind_;
  DispatchResult perform_result_;
  DispatchResult reconcile_result_;
};

class DispatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    now_ = cf::store::unix_now();

    RunRecord run;
    run.run_id = "R12";
    run.plan_id = "PLAN-X";
    run.state = "Created";
    run.config_json = "{}";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());
  }

  void TearDown() override {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string record_intent(const std::string& kind, const std::string& digest) {
    NewOperation intent;
    intent.idempotency_key =
        cf::idempotency_key("R12", "M3", kind, cf::sha256_hex(digest));
    intent.milestone_id = "M3";
    intent.kind = kind;
    intent.input_json = digest;
    intent.expected_json = "{}";
    const auto applied = store_->apply_transition("R12", "Created", "Executing", "{}",
                                                  {intent}, now_);
    EXPECT_TRUE(applied.has_value()) << applied.error().message;
    return intent.idempotency_key;
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  UnixTime now_ = 0;
};

TEST_F(DispatcherTest, ConfirmsPendingOperation) {
  const std::string key = record_intent("local_commit", "tree-abc");
  Dispatcher dispatcher(*store_, "R12", "orch-1");
  dispatcher.register_handler(std::make_unique<FakeHandler>(
      "local_commit", DispatchResult{DispatchOutcome::Confirmed, "{\"sha\":\"abc\"}", ""},
      DispatchResult{DispatchOutcome::Confirmed, "{}", ""}));

  const auto dispatched = dispatcher.dispatch_pending(0, now_);
  ASSERT_TRUE(dispatched.has_value()) << dispatched.error().message;
  EXPECT_EQ(*dispatched, 1u);

  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kConfirmed);
  EXPECT_EQ(op->result_json, "{\"sha\":\"abc\"}");
}

TEST_F(DispatcherTest, RetryReturnsOperationToPending) {
  const std::string key = record_intent("push", "commit-a");
  Dispatcher dispatcher(*store_, "R12", "orch-1");
  auto handler = std::make_unique<FakeHandler>(
      "push", DispatchResult{DispatchOutcome::Retry, "", "transient"},
      DispatchResult{DispatchOutcome::Retry, "", ""});
  FakeHandler* raw = handler.get();
  dispatcher.register_handler(std::move(handler));

  ASSERT_TRUE(dispatcher.dispatch_pending(0, now_).has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kPending);
  EXPECT_EQ(op->attempts, 1u);

  ASSERT_TRUE(dispatcher.dispatch_pending(0, now_ + 1).has_value());
  op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->attempts, 2u);
  EXPECT_EQ(raw->perform_calls, 2);
}

TEST_F(DispatcherTest, FailureMarksOperationFailed) {
  const std::string key = record_intent("pr_create", "branch-x");
  Dispatcher dispatcher(*store_, "R12", "orch-1");
  dispatcher.register_handler(std::make_unique<FakeHandler>(
      "pr_create", DispatchResult{DispatchOutcome::Failed, "", "permission denied"},
      DispatchResult{DispatchOutcome::Retry, "", ""}));

  ASSERT_TRUE(dispatcher.dispatch_pending(0, now_).has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kFailed);
  EXPECT_EQ(op->result_json, "permission denied");
}

TEST_F(DispatcherTest, MissingHandlerIsAnErrorAndLeavesIntentPending) {
  const std::string key = record_intent("index_publish", "index-v1");
  Dispatcher dispatcher(*store_, "R12", "orch-1");

  const auto dispatched = dispatcher.dispatch_pending(0, now_);
  ASSERT_FALSE(dispatched.has_value());
  EXPECT_EQ(dispatched.error().code, cf::orch::error_codes::kNoHandler);
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kPending);
}

TEST_F(DispatcherTest, DispatchRefusesStaleEpoch) {
  record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_)
                  .has_value());
  // A new orchestrator takes over after the old lease expires: epoch 2.
  const auto lease = store_->acquire_lease("R12", "orch-2", std::chrono::seconds(30),
                                           now_ + 100);
  ASSERT_TRUE(lease.has_value()) << lease.error().message;
  ASSERT_EQ(lease->epoch, 2u);

  Dispatcher dispatcher(*store_, "R12", "orch-1");
  dispatcher.register_handler(std::make_unique<FakeHandler>(
      "push", DispatchResult{DispatchOutcome::Confirmed, "{}", ""},
      DispatchResult{DispatchOutcome::Retry, "", ""}));

  const auto dispatched = dispatcher.dispatch_pending(1, now_);  // stale epoch
  ASSERT_FALSE(dispatched.has_value());
  EXPECT_EQ(dispatched.error().code, cf::store::error_codes::kStaleEpoch);
}

TEST_F(DispatcherTest, ReconcileStaleOperationAfterTakeover) {
  const std::string key = record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_)
                  .has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 1, now_).has_value());

  // A new orchestrator takes over after the old lease expires.
  auto lease = store_->acquire_lease("R12", "orch-2", std::chrono::seconds(30),
                                     now_ + 100);
  ASSERT_TRUE(lease.has_value()) << lease.error().message;
  ASSERT_EQ(lease->epoch, 2u);

  Dispatcher dispatcher(*store_, "R12", "orch-2");
  auto handler = std::make_unique<FakeHandler>(
      "push", DispatchResult{DispatchOutcome::Retry, "", ""},
      DispatchResult{DispatchOutcome::Confirmed, "{\"pushed\":true}", ""});
  FakeHandler* raw = handler.get();
  dispatcher.register_handler(std::move(handler));

  const auto reconciled = dispatcher.reconcile_stale(2, std::chrono::seconds(60),
                                                     now_ + 100);
  ASSERT_TRUE(reconciled.has_value()) << reconciled.error().message;
  EXPECT_EQ(*reconciled, 1u);
  EXPECT_EQ(raw->reconcile_calls, 1);

  op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kConfirmed);
  EXPECT_EQ(op->result_json, "{\"pushed\":true}");
  // The epoch column records the epoch that started the operation; the
  // confirmation was accepted under the current epoch (2).
  EXPECT_EQ(op->epoch, 1u);
}

TEST_F(DispatcherTest, ReconcileRespectsInFlightGracePeriod) {
  const std::string key = record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_)
                  .has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 1, now_).has_value());
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "orch-2", std::chrono::seconds(30), now_ + 100)
                  .has_value());

  Dispatcher dispatcher(*store_, "R12", "orch-2");
  auto handler = std::make_unique<FakeHandler>(
      "push", DispatchResult{DispatchOutcome::Retry, "", ""},
      DispatchResult{DispatchOutcome::Confirmed, "{}", ""});
  FakeHandler* raw = handler.get();
  dispatcher.register_handler(std::move(handler));

  // The old request may still be in flight: updated_at + grace > now.
  const auto reconciled = dispatcher.reconcile_stale(2, std::chrono::seconds(600),
                                                     now_ + 100);
  ASSERT_TRUE(reconciled.has_value());
  EXPECT_EQ(*reconciled, 0u);
  EXPECT_EQ(raw->reconcile_calls, 0);
  op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kStarted);
}

TEST_F(DispatcherTest, ReconcileSkipsCurrentEpochOperations) {
  const std::string key = record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_)
                  .has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 1, now_).has_value());

  Dispatcher dispatcher(*store_, "R12", "orch-1");
  auto handler = std::make_unique<FakeHandler>(
      "push", DispatchResult{DispatchOutcome::Retry, "", ""},
      DispatchResult{DispatchOutcome::Confirmed, "{}", ""});
  FakeHandler* raw = handler.get();
  dispatcher.register_handler(std::move(handler));

  const auto reconciled = dispatcher.reconcile_stale(1, std::chrono::seconds(0),
                                                     now_ + 1000);
  ASSERT_TRUE(reconciled.has_value());
  EXPECT_EQ(*reconciled, 0u);
  EXPECT_EQ(raw->reconcile_calls, 0);
}

}  // namespace
