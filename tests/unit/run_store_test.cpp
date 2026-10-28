// Unit tests for the run store and the side-effect protocol persistence
// (design sections 6.12, 8.4).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/common/hash.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::store::NewOperation;
using cf::store::OperationRecord;
using cf::store::RunRecord;
using cf::store::RunStore;
using cf::store::UnixTime;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_store_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class RunStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    now_ = cf::store::unix_now();
  }

  void TearDown() override {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void create_run(const std::string& run_id = "R12") {
    RunRecord run;
    run.run_id = run_id;
    run.plan_id = "PLAN-X";
    run.state = "Created";
    run.config_json = "{}";
    run.created_at = now_;
    const auto created = store_->create_run(run);
    ASSERT_TRUE(created.has_value()) << created.error().message;
  }

  NewOperation make_intent(const std::string& kind, const std::string& digest,
                           const std::string& milestone = "M3") {
    NewOperation intent;
    intent.idempotency_key =
        cf::idempotency_key("R12", milestone, kind, cf::sha256_hex(digest));
    intent.milestone_id = milestone;
    intent.kind = kind;
    intent.input_json = digest;
    intent.expected_json = "{}";
    return intent;
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  UnixTime now_ = 0;
};

TEST_F(RunStoreTest, CreateAndGetRun) {
  create_run();
  auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value()) << run.error().message;
  EXPECT_EQ(run->plan_id, "PLAN-X");
  EXPECT_EQ(run->state, "Created");
}

TEST_F(RunStoreTest, DuplicateRunIsConflict) {
  create_run();
  RunRecord duplicate;
  duplicate.run_id = "R12";
  duplicate.state = "Created";
  const auto created = store_->create_run(duplicate);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().code, cf::store::error_codes::kConflict);
}

TEST_F(RunStoreTest, MissingRunIsNotFound) {
  const auto run = store_->get_run("R99");
  ASSERT_FALSE(run.has_value());
  EXPECT_EQ(run.error().code, cf::store::error_codes::kNotFound);
}

TEST_F(RunStoreTest, TransitionUpdatesStateAndIsRecorded) {
  create_run();
  const auto applied = store_->apply_transition("R12", "Created", "Ingesting", "{}", {},
                                                now_ + 1);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, "Ingesting");
}

TEST_F(RunStoreTest, StaleTransitionIsRejected) {
  create_run();
  ASSERT_TRUE(store_->apply_transition("R12", "Created", "Ingesting", "{}", {}, now_)
                  .has_value());
  const auto stale = store_->apply_transition("R12", "Created", "Refining", "{}", {},
                                              now_);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().code, cf::store::error_codes::kInvalidTransition);
  auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, "Ingesting");
}

TEST_F(RunStoreTest, TransitionRecordsIntentAtomically) {
  create_run();
  const NewOperation intent = make_intent("local_commit", "tree-abc");
  const auto applied = store_->apply_transition("R12", "Created", "Executing", "{}",
                                                {intent}, now_ + 1);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  auto op = store_->find_operation_by_key(intent.idempotency_key);
  ASSERT_TRUE(op.has_value()) << op.error().message;
  EXPECT_EQ(op->state, cf::store::op_state::kPending);
  EXPECT_EQ(op->kind, "local_commit");
  EXPECT_EQ(op->run_id, "R12");
  EXPECT_EQ(op->milestone_id, "M3");
  EXPECT_EQ(op->attempts, 0u);
}

TEST_F(RunStoreTest, DuplicateIdempotencyKeyRollsBackTransition) {
  create_run();
  const NewOperation intent = make_intent("push", "commit-c3d4");
  ASSERT_TRUE(store_->apply_transition("R12", "Created", "Executing", "{}", {intent},
                                       now_)
                  .has_value());

  // A retry with the same idempotency key must not duplicate the intent, and
  // the whole transition must roll back.
  const auto retry = store_->apply_transition("R12", "Executing", "Done", "{}", {intent},
                                              now_ + 1);
  ASSERT_FALSE(retry.has_value());
  EXPECT_EQ(retry.error().code, cf::store::error_codes::kConflict);
  auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, "Executing");

  auto ops = store_->list_operations("R12", {});
  ASSERT_TRUE(ops.has_value());
  EXPECT_EQ(ops->size(), 1u);
}

TEST_F(RunStoreTest, OperationLifecycle) {
  create_run();
  const NewOperation intent = make_intent("local_commit", "tree-abc");
  ASSERT_TRUE(store_->apply_transition("R12", "Created", "Executing", "{}", {intent},
                                       now_)
                  .has_value());
  auto op = store_->find_operation_by_key(intent.idempotency_key);
  ASSERT_TRUE(op.has_value());

  const auto started = store_->mark_operation_started(op->op_id, 0, now_ + 1);
  ASSERT_TRUE(started.has_value()) << started.error().message;
  op = store_->get_operation(op->op_id);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kStarted);
  EXPECT_EQ(op->attempts, 1u);

  const auto confirmed = store_->confirm_operation(op->op_id, "{\"commit\":\"abc\"}", 0,
                                                   now_ + 2);
  ASSERT_TRUE(confirmed.has_value()) << confirmed.error().message;
  op = store_->get_operation(op->op_id);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kConfirmed);
  EXPECT_EQ(op->result_json, "{\"commit\":\"abc\"}");

  // Confirming twice is an invalid operation state.
  const auto again = store_->confirm_operation(op->op_id, "{}", 0, now_ + 3);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, cf::store::error_codes::kInvalidOperationState);
}

TEST_F(RunStoreTest, RetryAndFailureTransitions) {
  create_run();
  const NewOperation a = make_intent("push", "commit-a");
  const NewOperation b = make_intent("pr_create", "pr-b");
  ASSERT_TRUE(store_->apply_transition("R12", "Created", "Executing", "{}", {a, b}, now_)
                  .has_value());

  auto op_a = store_->find_operation_by_key(a.idempotency_key);
  auto op_b = store_->find_operation_by_key(b.idempotency_key);
  ASSERT_TRUE(op_a.has_value());
  ASSERT_TRUE(op_b.has_value());

  ASSERT_TRUE(store_->mark_operation_started(op_a->op_id, 0, now_).has_value());
  ASSERT_TRUE(store_->retry_operation(op_a->op_id, now_ + 1).has_value());
  op_a = store_->get_operation(op_a->op_id);
  ASSERT_TRUE(op_a.has_value());
  EXPECT_EQ(op_a->state, cf::store::op_state::kPending);
  EXPECT_EQ(op_a->attempts, 1u);

  ASSERT_TRUE(store_->mark_operation_started(op_b->op_id, 0, now_).has_value());
  ASSERT_TRUE(store_->fail_operation(op_b->op_id, "github rejected", now_ + 1)
                  .has_value());
  op_b = store_->get_operation(op_b->op_id);
  ASSERT_TRUE(op_b.has_value());
  EXPECT_EQ(op_b->state, cf::store::op_state::kFailed);
  EXPECT_EQ(op_b->result_json, "github rejected");
}

TEST_F(RunStoreTest, EpochFencingRejectsStaleEpoch) {
  create_run();
  const NewOperation intent = make_intent("push", "commit-c");
  ASSERT_TRUE(store_->apply_transition("R12", "Created", "Executing", "{}", {intent},
                                       now_)
                  .has_value());
  auto op = store_->find_operation_by_key(intent.idempotency_key);
  ASSERT_TRUE(op.has_value());

  auto lease = store_->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_);
  ASSERT_TRUE(lease.has_value()) << lease.error().message;
  ASSERT_EQ(lease->epoch, 1u);

  const auto stale_start = store_->mark_operation_started(op->op_id, 0, now_);
  ASSERT_FALSE(stale_start.has_value());
  EXPECT_EQ(stale_start.error().code, cf::store::error_codes::kStaleEpoch);

  ASSERT_TRUE(store_->mark_operation_started(op->op_id, 1, now_).has_value());
  const auto stale_confirm = store_->confirm_operation(op->op_id, "{}", 0, now_);
  ASSERT_FALSE(stale_confirm.has_value());
  EXPECT_EQ(stale_confirm.error().code, cf::store::error_codes::kStaleEpoch);
  ASSERT_TRUE(store_->confirm_operation(op->op_id, "{}", 1, now_).has_value());
}

TEST_F(RunStoreTest, LeaseAcquireRenewRelease) {
  create_run();
  auto lease = store_->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_);
  ASSERT_TRUE(lease.has_value()) << lease.error().message;
  EXPECT_EQ(lease->holder, "orch-1");
  EXPECT_EQ(lease->epoch, 1u);

  ASSERT_TRUE(store_->renew_lease("R12", "orch-1", 1, now_ + 10).has_value());

  const auto wrong_holder = store_->renew_lease("R12", "orch-2", 1, now_ + 11);
  ASSERT_FALSE(wrong_holder.has_value());
  EXPECT_EQ(wrong_holder.error().code, cf::store::error_codes::kLeaseLost);

  ASSERT_TRUE(store_->release_lease("R12", "orch-1", 1).has_value());
  const auto after_release = store_->get_lease("R12");
  ASSERT_TRUE(after_release.has_value());
  EXPECT_TRUE(after_release->holder.empty());  // row kept for epoch monotonicity
  EXPECT_EQ(after_release->epoch, 1u);
}

TEST_F(RunStoreTest, ReacquiringOwnLiveLeaseReturnsItUnchanged) {
  create_run();
  auto first = store_->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_);
  ASSERT_TRUE(first.has_value());
  auto second = store_->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_ + 1);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->epoch, first->epoch);
}

TEST_F(RunStoreTest, ExpiredLeaseIsTakenOverWithHigherEpoch) {
  create_run();
  auto first = store_->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_);
  ASSERT_TRUE(first.has_value());
  auto second =
      store_->acquire_lease("R12", "orch-2", std::chrono::seconds(30), now_ + 100);
  ASSERT_TRUE(second.has_value()) << second.error().message;
  EXPECT_EQ(second->holder, "orch-2");
  EXPECT_EQ(second->epoch, 2u);
}

TEST_F(RunStoreTest, SecondStoreCannotAcquireLiveFileLock) {
  create_run();
  auto lease = store_->acquire_lease("R12", "orch-1", std::chrono::seconds(30), now_);
  ASSERT_TRUE(lease.has_value());

  auto opened = RunStore::open(dir_ / "run.db");
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<RunStore> other = std::move(*opened);
  const auto blocked =
      other->acquire_lease("R12", "orch-2", std::chrono::seconds(30), now_);
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(blocked.error().code, cf::store::error_codes::kLeaseHeld);

  // After the first holder releases, the second can take over.
  ASSERT_TRUE(store_->release_lease("R12", "orch-1", 1).has_value());
  const auto acquired =
      other->acquire_lease("R12", "orch-2", std::chrono::seconds(30), now_ + 1);
  ASSERT_TRUE(acquired.has_value()) << acquired.error().message;
  EXPECT_EQ(acquired->epoch, 2u);
}

TEST_F(RunStoreTest, CheckpointsLatestWins) {
  create_run();
  cf::store::CheckpointRecord first;
  first.run_id = "R12";
  first.milestone_id = "M3";
  first.worktree_heads_json = "{\"coder\":\"aaa\"}";
  first.sessions_json = "[]";
  first.milestone_state = "Coding";
  first.at = now_;
  ASSERT_TRUE(store_->save_checkpoint(first).has_value());

  cf::store::CheckpointRecord second;
  second.run_id = "R12";
  second.milestone_id = "M3";
  second.worktree_heads_json = "{\"coder\":\"bbb\"}";
  second.sessions_json = "[]";
  second.milestone_state = "Gating";
  second.at = now_ + 5;
  ASSERT_TRUE(store_->save_checkpoint(second).has_value());

  auto latest = store_->latest_checkpoint("R12", "M3");
  ASSERT_TRUE(latest.has_value()) << latest.error().message;
  EXPECT_EQ(latest->worktree_heads_json, "{\"coder\":\"bbb\"}");
  EXPECT_EQ(latest->milestone_state, "Gating");
}

TEST_F(RunStoreTest, LlmCallLedgerTotals) {
  create_run();
  cf::store::LlmCallRecord call;
  call.run_id = "R12";
  call.role = "coder";
  call.model = "accounts/fireworks/models/deepseek-v4p1-flash";
  call.prompt_tokens = 1000;
  call.completion_tokens = 200;
  call.cached_tokens = 500;
  call.usd = 0.25;
  call.latency_ms = 1200;
  call.at = now_;
  ASSERT_TRUE(store_->record_llm_call(call).has_value());
  call.prompt_tokens = 300;
  call.completion_tokens = 50;
  call.usd = 0.10;
  ASSERT_TRUE(store_->record_llm_call(call).has_value());

  auto usd = store_->total_usd("R12");
  ASSERT_TRUE(usd.has_value());
  EXPECT_NEAR(*usd, 0.35, 1e-9);
  auto tokens = store_->total_tokens("R12");
  ASSERT_TRUE(tokens.has_value());
  EXPECT_EQ(*tokens, 1550);
}

TEST_F(RunStoreTest, MilestoneStatePersists) {
  create_run();
  cf::store::MilestoneRecord milestone;
  milestone.run_id = "R12";
  milestone.milestone_id = "M3";
  milestone.state = "TestAuthoring";
  milestone.iterations = 0;
  ASSERT_TRUE(store_->upsert_milestone(milestone).has_value());

  ASSERT_TRUE(store_->set_milestone_state("R12", "M3", "Coding", 1, now_).has_value());
  auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value()) << stored.error().message;
  EXPECT_EQ(stored->state, "Coding");
  EXPECT_EQ(stored->iterations, 1u);
}

TEST_F(RunStoreTest, LockAcquireGetListRelease) {
  create_run();
  const std::string lock_id = cf::store::integration_lock_id();
  auto lock = store_->acquire_lock(lock_id, "R12", cf::store::lock_kind::kIntegration,
                                   "M3", 1, now_);
  ASSERT_TRUE(lock.has_value()) << lock.error().message;
  EXPECT_EQ(lock->lock_id, "integration");
  EXPECT_EQ(lock->holder_milestone, "M3");
  EXPECT_EQ(lock->epoch, 1u);

  auto fetched = store_->get_lock(lock_id);
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(fetched->kind, cf::store::lock_kind::kIntegration);

  auto locks = store_->list_locks("R12");
  ASSERT_TRUE(locks.has_value());
  ASSERT_EQ(locks->size(), 1u);
  EXPECT_EQ((*locks)[0].holder_milestone, "M3");

  ASSERT_TRUE(store_->release_lock(lock_id, "M3", 1).has_value());
  const auto gone = store_->get_lock(lock_id);
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error().code, cf::store::error_codes::kLockNotFound);
}

TEST_F(RunStoreTest, LockRejectsOtherHolder) {
  create_run();
  const std::string lock_id = cf::store::interface_lock_id("IFoo");
  ASSERT_TRUE(store_
                  ->acquire_lock(lock_id, "R12", cf::store::lock_kind::kInterface, "M3",
                                 1, now_)
                  .has_value());
  const auto conflict = store_->acquire_lock(
      lock_id, "R12", cf::store::lock_kind::kInterface, "M4", 1, now_);
  ASSERT_FALSE(conflict.has_value());
  EXPECT_EQ(conflict.error().code, cf::store::error_codes::kLockHeld);
}

TEST_F(RunStoreTest, LockReacquireBySameHolderIsIdempotent) {
  create_run();
  const std::string lock_id = cf::store::interface_lock_id("IFoo");
  ASSERT_TRUE(store_
                  ->acquire_lock(lock_id, "R12", cf::store::lock_kind::kInterface, "M3",
                                 1, now_)
                  .has_value());
  const auto again = store_->acquire_lock(
      lock_id, "R12", cf::store::lock_kind::kInterface, "M3", 1, now_ + 5);
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_EQ(again->acquired_at, now_);  // the original acquisition is returned

  // The same milestone under a newer epoch is still refused: recovery must
  // release the stale lock first (release_stale_locks).
  const auto new_epoch = store_->acquire_lock(
      lock_id, "R12", cf::store::lock_kind::kInterface, "M3", 2, now_ + 5);
  ASSERT_FALSE(new_epoch.has_value());
  EXPECT_EQ(new_epoch.error().code, cf::store::error_codes::kLockHeld);
}

TEST_F(RunStoreTest, ReleaseLockByWrongHolderFails) {
  create_run();
  const std::string lock_id = cf::store::interface_lock_id("IFoo");
  ASSERT_TRUE(store_
                  ->acquire_lock(lock_id, "R12", cf::store::lock_kind::kInterface, "M3",
                                 1, now_)
                  .has_value());
  const auto released = store_->release_lock(lock_id, "M4", 1);
  ASSERT_FALSE(released.has_value());
  EXPECT_EQ(released.error().code, cf::store::error_codes::kLockNotFound);
}

TEST_F(RunStoreTest, ReleaseStaleLocksOnTakeover) {
  create_run();
  ASSERT_TRUE(store_
                  ->acquire_lock(cf::store::integration_lock_id(), "R12",
                                 cf::store::lock_kind::kIntegration, "M3", 1, now_)
                  .has_value());
  ASSERT_TRUE(store_
                  ->acquire_lock(cf::store::interface_lock_id("IFoo"), "R12",
                                 cf::store::lock_kind::kInterface, "M4", 1, now_)
                  .has_value());
  ASSERT_TRUE(store_
                  ->acquire_lock(cf::store::interface_lock_id("IBar"), "R12",
                                 cf::store::lock_kind::kInterface, "M5", 2, now_)
                  .has_value());

  const auto released = store_->release_stale_locks("R12", 2);
  ASSERT_TRUE(released.has_value()) << released.error().message;
  EXPECT_EQ(*released, 2u);

  auto locks = store_->list_locks("R12");
  ASSERT_TRUE(locks.has_value());
  ASSERT_EQ(locks->size(), 1u);
  EXPECT_EQ((*locks)[0].holder_milestone, "M5");
  EXPECT_EQ((*locks)[0].epoch, 2u);
}

TEST_F(RunStoreTest, LockIdHelpers) {
  EXPECT_EQ(cf::store::integration_lock_id(), "integration");
  EXPECT_EQ(cf::store::interface_lock_id("IFoo"), "interface:IFoo");
}

TEST_F(RunStoreTest, DocVersionsRoundTrip) {
  cf::store::DocVersionRow row;
  row.doc_id = "CG";
  row.version = "3.2";
  row.sha256 = "abc";
  row.precedence = 4;
  row.ingested_at = now_;
  ASSERT_TRUE(store_->upsert_doc_version(row).has_value());

  row.sha256 = "def";  // re-ingesting the same version replaces it
  ASSERT_TRUE(store_->upsert_doc_version(row).has_value());

  auto versions = store_->list_doc_versions();
  ASSERT_TRUE(versions.has_value()) << versions.error().message;
  ASSERT_EQ(versions->size(), 1u);
  EXPECT_EQ((*versions)[0].sha256, "def");

  auto fetched = store_->get_doc_version("CG", "3.2");
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(fetched->precedence, 4);
  EXPECT_EQ(fetched->ingested_at, now_);

  const auto missing = store_->get_doc_version("CG", "9.9");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, cf::store::error_codes::kNotFound);
}

TEST_F(RunStoreTest, RecordIntentsWithoutATransition) {
  create_run();
  cf::store::NewOperation first;
  first.idempotency_key = cf::idempotency_key("R12", "", "push", "abc");
  first.kind = "push";
  first.input_json = "{}";
  first.expected_json = "{}";
  ASSERT_TRUE(store_->record_intents("R12", {first}, now_).has_value());

  auto op = store_->find_operation_by_key(first.idempotency_key);
  ASSERT_TRUE(op.has_value());
  EXPECT_EQ(op->state, cf::store::op_state::kPending);

  const auto duplicate = store_->record_intents("R12", {first}, now_);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code, cf::store::error_codes::kConflict);
}

TEST_F(RunStoreTest, ListsMilestonesAndTransitions) {
  create_run();
  ASSERT_TRUE(store_->apply_transition("R12", "Created", "Ingesting", "{}", {}, now_)
                  .has_value());
  ASSERT_TRUE(store_->apply_transition("R12", "Ingesting", "Refining", "{}", {}, now_ + 1)
                  .has_value());

  cf::store::MilestoneRecord milestone;
  milestone.run_id = "R12";
  milestone.milestone_id = "M2";
  milestone.state = "Pending";
  ASSERT_TRUE(store_->upsert_milestone(milestone).has_value());
  milestone.milestone_id = "M1";
  ASSERT_TRUE(store_->upsert_milestone(milestone).has_value());

  auto milestones = store_->list_milestones("R12");
  ASSERT_TRUE(milestones.has_value()) << milestones.error().message;
  ASSERT_EQ(milestones->size(), 2u);
  EXPECT_EQ((*milestones)[0].milestone_id, "M1");  // ordered by id
  EXPECT_EQ((*milestones)[1].milestone_id, "M2");

  auto transitions = store_->list_transitions("R12");
  ASSERT_TRUE(transitions.has_value()) << transitions.error().message;
  ASSERT_EQ(transitions->size(), 2u);
  EXPECT_EQ((*transitions)[0].from_state, "Created");
  EXPECT_EQ((*transitions)[1].to_state, "Refining");

  auto recent = store_->list_transitions("R12", 1);
  ASSERT_TRUE(recent.has_value());
  ASSERT_EQ(recent->size(), 1u);
  EXPECT_EQ((*recent)[0].to_state, "Refining");
}

TEST_F(RunStoreTest, WaiversPersistAndAreIdempotent) {
  create_run();
  cf::store::WaiverRecord waiver;
  waiver.run_id = "R12";
  waiver.milestone_id = "M3";
  waiver.rule = "guardrails";
  waiver.scope = "src/bus.cpp:42";
  waiver.justification = "Legacy API kept for the compatibility window";
  waiver.approved_by = "alice";
  waiver.at = now_;
  auto recorded = store_->record_waiver(waiver);
  ASSERT_TRUE(recorded.has_value()) << recorded.error().message;
  EXPECT_FALSE(recorded->waiver_id.empty());
  EXPECT_EQ(recorded->approved_by, "alice");

  // A retried command returns the existing human decision.
  waiver.justification = "a different text";
  auto again = store_->record_waiver(waiver);
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_EQ(again->waiver_id, recorded->waiver_id);
  EXPECT_EQ(again->justification,
            "Legacy API kept for the compatibility window");

  cf::store::WaiverRecord run_wide;
  run_wide.run_id = "R12";
  run_wide.rule = "format";
  run_wide.scope = "src/registry.cpp:9";
  run_wide.justification = "Generated file, formatting is owned upstream";
  run_wide.approved_by = "bob";
  run_wide.at = now_ + 1;
  ASSERT_TRUE(store_->record_waiver(run_wide).has_value());

  auto all = store_->list_waivers("R12");
  ASSERT_TRUE(all.has_value()) << all.error().message;
  ASSERT_EQ(all->size(), 2u);
  EXPECT_EQ((*all)[0].rule, "guardrails");

  auto milestone = store_->list_waivers("R12", "M3");
  ASSERT_TRUE(milestone.has_value());
  ASSERT_EQ(milestone->size(), 2u);  // its own plus the run-wide waiver
  auto other = store_->list_waivers("R12", "M1");
  ASSERT_TRUE(other.has_value());
  ASSERT_EQ(other->size(), 1u);
  EXPECT_EQ((*other)[0].rule, "format");

  // A waiver needs an existing run.
  cf::store::WaiverRecord orphan;
  orphan.run_id = "R99";
  orphan.rule = "build";
  orphan.scope = "src/x.cpp:1";
  orphan.justification = "no";
  orphan.approved_by = "alice";
  const auto missing = store_->record_waiver(orphan);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, cf::store::error_codes::kNotFound);
}

TEST_F(RunStoreTest, WaiverRequestsRoundTripAndExpire) {
  create_run();
  cf::store::WaiverRequestRecord request;
  request.run_id = "R12";
  request.milestone_id = "M3";
  request.rule = "guardrails";
  request.scope = "src/a.cpp:1";
  request.justification = "legacy API";
  request.requested_by = "coder";
  request.requested_at = now_;
  request.deadline_at = now_ + 100;
  auto recorded = store_->record_waiver_request(request);
  ASSERT_TRUE(recorded.has_value()) << recorded.error().message;
  EXPECT_EQ(recorded->status, "PENDING");
  EXPECT_FALSE(recorded->id.empty());

  auto again = store_->record_waiver_request(request);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->id, recorded->id);

  auto pending = store_->list_waiver_requests("R12");
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(pending->size(), 1u);
  EXPECT_EQ((*pending)[0].requested_by, "coder");
  auto fetched = store_->get_waiver_request(recorded->id);
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(fetched->rule, "guardrails");

  auto expired = store_->expire_waiver_requests("R12", now_ + 101);
  ASSERT_TRUE(expired.has_value());
  EXPECT_EQ(*expired, 1u);
  auto after = store_->list_waiver_requests("R12");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ((*after)[0].status, "EXPIRED");

  // A human decision updates a pending request.
  request.rule = "format";
  request.scope = "src/b.cpp:2";
  request.deadline_at = now_ + 500;
  auto second = store_->record_waiver_request(request);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(store_
                  ->decide_waiver_requests("R12", "M3", "format", "src/b.cpp:2",
                                           "APPROVED", now_ + 200)
                  .has_value());
  auto decided = store_->list_waiver_requests("R12", "M3");
  ASSERT_TRUE(decided.has_value());
  bool approved = false;
  for (const auto& row : *decided) {
    approved = approved || (row.rule == "format" && row.status == "APPROVED");
  }
  EXPECT_TRUE(approved);
}

TEST_F(RunStoreTest, GateResultsPersistAndAggregate) {
  create_run();
  cf::store::GateResultRow row;
  row.run_id = "R12";
  row.milestone_id = "M3";
  row.commit_sha = "a1b2c3d";
  row.gate = "build";
  row.passed = true;
  row.report_ref = "artifact://runs/R12/M3/gates.json";
  row.at = now_;
  ASSERT_TRUE(store_->record_gate_result(row).has_value());
  row.gate = "format";
  ASSERT_TRUE(store_->record_gate_result(row).has_value());

  auto results = store_->list_gate_results("R12", "a1b2c3d");
  ASSERT_TRUE(results.has_value()) << results.error().message;
  ASSERT_EQ(results->size(), 2u);
  EXPECT_EQ((*results)[0].gate, "build");
  EXPECT_TRUE((*results)[0].passed);

  auto passed = store_->all_gates_passed("R12", "a1b2c3d");
  ASSERT_TRUE(passed.has_value());
  EXPECT_TRUE(*passed);

  row.gate = "guardrails";
  row.passed = false;
  ASSERT_TRUE(store_->record_gate_result(row).has_value());
  passed = store_->all_gates_passed("R12", "a1b2c3d");
  ASSERT_TRUE(passed.has_value());
  EXPECT_FALSE(*passed);

  // A commit with no gate results has not passed.
  passed = store_->all_gates_passed("R12", "deadbeef");
  ASSERT_TRUE(passed.has_value());
  EXPECT_FALSE(*passed);
}

}  // namespace
