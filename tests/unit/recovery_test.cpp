// Unit tests for crash recovery (design section 6.12.4).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/common/hash.hpp"
#include "cf/orch/recovery.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::DispatchOutcome;
using cf::orch::Dispatcher;
using cf::orch::DispatchResult;
using cf::orch::GitWorktreeHook;
using cf::orch::OperationHandler;
using cf::orch::Recovery;
using cf::orch::RecoveryHook;
using cf::orch::RecoveryOptions;
using cf::store::CheckpointRecord;
using cf::store::NewOperation;
using cf::store::OperationRecord;
using cf::store::RunRecord;
using cf::store::RunStore;
using cf::store::UnixTime;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_recovery_test_" + std::to_string(::getpid()) + "_" +
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
    (void)op;
    return perform_result_;
  }
  DispatchResult reconcile(const OperationRecord& op) override {
    (void)op;
    ++reconcile_calls;
    return reconcile_result_;
  }

  int reconcile_calls = 0;

 private:
  std::string kind_;
  DispatchResult perform_result_;
  DispatchResult reconcile_result_;
};

class FakeHook final : public RecoveryHook {
 public:
  cf::Status reset_worktrees(const CheckpointRecord& checkpoint) override {
    seen.push_back(checkpoint);
    return {};
  }
  std::vector<CheckpointRecord> seen;
};

class RecoveryTest : public ::testing::Test {
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
    ASSERT_TRUE(store_->apply_transition("R12", "Created", "Executing", "{}", {}, now_)
                    .has_value());
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
    const auto applied = store_->apply_transition("R12", "Executing", "Executing", "{}",
                                                  {intent}, now_);
    EXPECT_TRUE(applied.has_value()) << applied.error().message;
    return intent.idempotency_key;
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  UnixTime now_ = 0;
};

TEST_F(RecoveryTest, TakesOverExpiredLeaseAndReleasesStaleLocks) {
  record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now_ - 1000)
                  .has_value());
  ASSERT_TRUE(store_
                  ->acquire_lock(cf::store::integration_lock_id(), "R12",
                                 cf::store::lock_kind::kIntegration, "M3", 1, now_ - 1000)
                  .has_value());

  Recovery recovery(*store_);
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  RecoveryOptions options;
  options.holder = "new-orch";
  options.in_flight_grace = std::chrono::seconds(0);
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->epoch, 2u);
  EXPECT_EQ(report->locks_released, 1u);

  auto locks = store_->list_locks("R12");
  ASSERT_TRUE(locks.has_value());
  EXPECT_TRUE(locks->empty());
}

TEST_F(RecoveryTest, FailsWhenAnotherOrchestratorHoldsTheLease) {
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "other-orch", std::chrono::seconds(300), now_)
                  .has_value());
  Recovery recovery(*store_);
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  RecoveryOptions options;
  options.holder = "new-orch";
  const auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().code, cf::store::error_codes::kLeaseHeld);
}

TEST_F(RecoveryTest, ReconcilesStartedOperationsPastTheGracePeriod) {
  const std::string key = record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now_ - 1000)
                  .has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 1, now_ - 1000).has_value());

  auto handler = std::make_unique<FakeHandler>(
      "push", DispatchResult{DispatchOutcome::Retry, "", ""},
      DispatchResult{DispatchOutcome::Confirmed, "{\"pushed\":true}", ""});
  FakeHandler* raw = handler.get();
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  dispatcher.register_handler(std::move(handler));

  Recovery recovery(*store_);
  RecoveryOptions options;
  options.holder = "new-orch";
  options.in_flight_grace = std::chrono::seconds(60);
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->started_reconciled, 1u);
  EXPECT_EQ(raw->reconcile_calls, 1);

  op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kConfirmed);
}

TEST_F(RecoveryTest, RespectsTheInFlightGracePeriod) {
  const std::string key = record_intent("push", "commit-a");
  ASSERT_TRUE(store_
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now_ - 1000)
                  .has_value());
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 1, now_).has_value());

  Dispatcher dispatcher(*store_, "R12", "new-orch");
  Recovery recovery(*store_);
  RecoveryOptions options;
  options.holder = "new-orch";
  options.in_flight_grace = std::chrono::seconds(600);
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->started_reconciled, 0u);

  op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kStarted);
}

TEST_F(RecoveryTest, RetriesOperationsThatNeedReconciliation) {
  const std::string key = record_intent("pr_create", "branch-x");
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 0, now_).has_value());
  ASSERT_TRUE(store_->mark_operation_needs_reconcile(op->op_id, "world differs", now_)
                  .has_value());

  auto handler = std::make_unique<FakeHandler>(
      "pr_create", DispatchResult{DispatchOutcome::Confirmed, "{}", ""},
      DispatchResult{DispatchOutcome::Retry, "", ""});
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  dispatcher.register_handler(std::move(handler));

  Recovery recovery(*store_);
  RecoveryOptions options;
  options.holder = "new-orch";
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->needs_reconcile_retried, 1u);
  EXPECT_TRUE(report->still_needs_reconcile.empty());

  op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kPending);
}

TEST_F(RecoveryTest, ReportsOperationsThatStillNeedReconciliation) {
  const std::string key = record_intent("pr_create", "branch-x");
  auto op = store_->find_operation_by_key(key);
  ASSERT_TRUE(op.has_value());
  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 0, now_).has_value());
  ASSERT_TRUE(store_->mark_operation_needs_reconcile(op->op_id, "world differs", now_)
                  .has_value());

  auto handler = std::make_unique<FakeHandler>(
      "pr_create", DispatchResult{DispatchOutcome::Confirmed, "{}", ""},
      DispatchResult{DispatchOutcome::NeedsReconcile, "", "still unclear"});
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  dispatcher.register_handler(std::move(handler));

  Recovery recovery(*store_);
  RecoveryOptions options;
  options.holder = "new-orch";
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->needs_reconcile_retried, 1u);
  ASSERT_EQ(report->still_needs_reconcile.size(), 1u);
  EXPECT_EQ(report->still_needs_reconcile[0], op->op_id);
}

TEST_F(RecoveryTest, CallsTheWorktreeHookWithTheLatestCheckpoint) {
  cf::store::MilestoneRecord milestone;
  milestone.run_id = "R12";
  milestone.milestone_id = "M3";
  milestone.state = "Coding";
  ASSERT_TRUE(store_->upsert_milestone(milestone).has_value());

  CheckpointRecord first;
  first.run_id = "R12";
  first.milestone_id = "M3";
  first.worktree_heads_json = "{\"coder\":\"aaa\"}";
  first.milestone_state = "Coding";
  first.at = now_;
  ASSERT_TRUE(store_->save_checkpoint(first).has_value());
  CheckpointRecord second = first;
  second.worktree_heads_json = "{\"coder\":\"bbb\"}";
  second.at = now_ + 10;
  ASSERT_TRUE(store_->save_checkpoint(second).has_value());

  auto hook = std::make_unique<FakeHook>();
  FakeHook* raw = hook.get();
  Recovery recovery(*store_, std::move(hook));
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  RecoveryOptions options;
  options.holder = "new-orch";
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->worktrees_reset, 1u);
  ASSERT_EQ(raw->seen.size(), 1u);
  EXPECT_EQ(raw->seen[0].worktree_heads_json, "{\"coder\":\"bbb\"}");
}

TEST_F(RecoveryTest, DerivesMilestoneStatesAndSerializesTheReport) {
  cf::store::MilestoneRecord done;
  done.run_id = "R12";
  done.milestone_id = "M1";
  done.state = "Done";
  ASSERT_TRUE(store_->upsert_milestone(done).has_value());
  cf::store::MilestoneRecord coding;
  coding.run_id = "R12";
  coding.milestone_id = "M2";
  coding.state = "Coding";
  coding.iterations = 3;
  ASSERT_TRUE(store_->upsert_milestone(coding).has_value());

  Recovery recovery(*store_);
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  RecoveryOptions options;
  options.holder = "new-orch";
  auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  ASSERT_EQ(report->milestones.size(), 2u);

  const auto json = nlohmann::json::parse(report->to_json());
  EXPECT_EQ(json["milestones"].size(), 2u);
  EXPECT_EQ(json["milestones"][1]["state"], "Coding");
  EXPECT_EQ(json["milestones"][1]["iterations"], 3);
}

TEST_F(RecoveryTest, UnknownStoredMilestoneStateFails) {
  cf::store::MilestoneRecord broken;
  broken.run_id = "R12";
  broken.milestone_id = "M9";
  broken.state = "Nonsense";
  ASSERT_TRUE(store_->upsert_milestone(broken).has_value());

  Recovery recovery(*store_);
  Dispatcher dispatcher(*store_, "R12", "new-orch");
  RecoveryOptions options;
  options.holder = "new-orch";
  const auto report = recovery.resume("R12", dispatcher, options, now_);
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().code, cf::orch::error_codes::kInvalidMilestoneTransition);
}

TEST(GitWorktreeHookTest, ResetsTheWorktreeAndParksRecoveredWork) {
  const auto dir = make_temp_dir();
  cf::process::LocalProcessExecutor executor;
  auto created = cf::git::Repository::create(dir / "repo", executor);
  ASSERT_TRUE(created.has_value()) << created.error().message;
  cf::git::Repository repository = std::move(*created);

  std::ofstream(dir / "repo" / "README.md") << "initial\n";
  cf::git::CommitTrailers init;
  init.run_id = "R12";
  init.op_id = "op_init";
  auto initial = repository.commit_paths(dir / "repo", {"README.md"}, "chore: init", init);
  ASSERT_TRUE(initial.has_value()) << initial.error().message;

  const auto workspace = dir / "workspace";
  const auto worktree = workspace / "wt" / "R12" / "M3" / "coder";
  ASSERT_TRUE(repository.create_worktree(worktree, "cf-local/R12/M3/coder").has_value());

  // Work that happened after the checkpoint, plus uncommitted partial edits.
  std::ofstream(worktree / "feature.cpp") << "feature\n";
  cf::git::CommitTrailers later;
  later.run_id = "R12";
  later.milestone_id = "M3";
  later.op_id = "op_later";
  ASSERT_TRUE(repository.commit_paths(worktree, {"feature.cpp"}, "feat: feature", later)
                  .has_value());
  std::ofstream(worktree / "scratch.txt") << "partial work\n";

  CheckpointRecord checkpoint;
  checkpoint.run_id = "R12";
  checkpoint.milestone_id = "M3";
  checkpoint.worktree_heads_json = "{\"coder\":\"" + *initial + "\"}";
  checkpoint.at = 100;

  GitWorktreeHook hook(repository, workspace);
  ASSERT_TRUE(hook.reset_worktrees(checkpoint).has_value());

  auto head = repository.head_sha(worktree);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(*head, *initial);
  EXPECT_FALSE(std::filesystem::exists(worktree / "scratch.txt"));
  EXPECT_FALSE(std::filesystem::exists(worktree / "feature.cpp"));

  auto parked = repository.head_sha(worktree, "refs/cf/recovered/R12/100/coder");
  ASSERT_TRUE(parked.has_value()) << parked.error().message;
  EXPECT_TRUE(repository.file_hash(*parked, "feature.cpp").has_value());
  EXPECT_TRUE(repository.file_hash(*parked, "scratch.txt").has_value());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(GitWorktreeHookTest, SkipsMissingWorktrees) {
  const auto dir = make_temp_dir();
  cf::process::LocalProcessExecutor executor;
  auto created = cf::git::Repository::create(dir / "repo", executor);
  ASSERT_TRUE(created.has_value());
  cf::git::Repository repository = std::move(*created);

  CheckpointRecord checkpoint;
  checkpoint.run_id = "R12";
  checkpoint.milestone_id = "M3";
  checkpoint.worktree_heads_json = "{\"coder\":\"deadbeef\"}";
  checkpoint.at = 100;

  GitWorktreeHook hook(repository, dir / "workspace");
  EXPECT_TRUE(hook.reset_worktrees(checkpoint).has_value());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
