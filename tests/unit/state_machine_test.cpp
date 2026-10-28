// Unit tests for the run and milestone state machines (design section 7).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/common/hash.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::MilestoneEvent;
using cf::orch::MilestoneState;
using cf::orch::RunEvent;
using cf::orch::RunState;
using cf::orch::StateMachine;
using cf::store::NewOperation;
using cf::store::RunRecord;
using cf::store::RunStore;
using cf::store::UnixTime;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_sm_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class StateMachineTest : public ::testing::Test {
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

  void apply_run(StateMachine& machine, RunEvent event) {
    const auto applied = machine.apply(event, {}, now_);
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
  }

  void apply_milestone(StateMachine& machine, const std::string& id,
                       MilestoneEvent event) {
    const auto applied = machine.apply(id, event, now_);
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  UnixTime now_ = 0;
};

TEST_F(StateMachineTest, RunHappyPath) {
  StateMachine machine(*store_, "R12");
  EXPECT_EQ(machine.current(), RunState::Created);
  apply_run(machine, RunEvent::Start);
  EXPECT_EQ(machine.current(), RunState::Ingesting);
  apply_run(machine, RunEvent::IndexReady);
  EXPECT_EQ(machine.current(), RunState::Refining);
  apply_run(machine, RunEvent::DesignPackageReady);
  EXPECT_EQ(machine.current(), RunState::AwaitingDesignApproval);
  apply_run(machine, RunEvent::Approved);
  EXPECT_EQ(machine.current(), RunState::Executing);
  apply_run(machine, RunEvent::AllMilestonesDone);
  EXPECT_EQ(machine.current(), RunState::SystemValidation);
  apply_run(machine, RunEvent::SystemValidationPassed);
  EXPECT_EQ(machine.current(), RunState::Done);

  const auto stored = store_->get_run("R12");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->state, "Done");
}

TEST_F(StateMachineTest, RunDesignChangesAndSystemDefects) {
  StateMachine machine(*store_, "R12");
  apply_run(machine, RunEvent::Start);
  apply_run(machine, RunEvent::IndexReady);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::ChangesRequested);
  EXPECT_EQ(machine.current(), RunState::Refining);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::Approved);
  apply_run(machine, RunEvent::AllMilestonesDone);
  apply_run(machine, RunEvent::SystemDefectsFiled);
  EXPECT_EQ(machine.current(), RunState::Executing);
}

TEST_F(StateMachineTest, RunEscalationAndHumanResolve) {
  StateMachine machine(*store_, "R12");
  apply_run(machine, RunEvent::Start);
  apply_run(machine, RunEvent::IndexReady);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::Approved);
  apply_run(machine, RunEvent::NoProgress);
  EXPECT_EQ(machine.current(), RunState::Escalated);
  apply_run(machine, RunEvent::HumanResolved);
  EXPECT_EQ(machine.current(), RunState::Executing);
}

TEST_F(StateMachineTest, RunDeadlineExpiryAborts) {
  StateMachine machine(*store_, "R12");
  apply_run(machine, RunEvent::Start);
  apply_run(machine, RunEvent::IndexReady);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::ApprovalDeadlineExpired);
  EXPECT_EQ(machine.current(), RunState::Aborted);
}

TEST_F(StateMachineTest, RunEscalationDeadlineAborts) {
  StateMachine machine(*store_, "R12");
  apply_run(machine, RunEvent::Start);
  apply_run(machine, RunEvent::IndexReady);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::Approved);
  apply_run(machine, RunEvent::BudgetExceeded);
  apply_run(machine, RunEvent::EscalationDeadlineExpired);
  EXPECT_EQ(machine.current(), RunState::Aborted);
}

TEST_F(StateMachineTest, InvalidRunEventIsRejected) {
  StateMachine machine(*store_, "R12");
  const auto applied = machine.apply(RunEvent::Approved, {}, now_);
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, cf::orch::error_codes::kInvalidRunTransition);
  EXPECT_EQ(machine.current(), RunState::Created);
}

TEST_F(StateMachineTest, TerminalRunStateRejectsEvents) {
  StateMachine machine(*store_, "R12");
  apply_run(machine, RunEvent::Start);
  apply_run(machine, RunEvent::IndexReady);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::Approved);
  apply_run(machine, RunEvent::AllMilestonesDone);
  apply_run(machine, RunEvent::SystemValidationPassed);
  const auto applied = machine.apply(RunEvent::Start, {}, now_);
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, cf::orch::error_codes::kInvalidRunTransition);
}

TEST_F(StateMachineTest, RunTransitionRecordsIntents) {
  StateMachine machine(*store_, "R12");
  apply_run(machine, RunEvent::Start);
  apply_run(machine, RunEvent::IndexReady);
  apply_run(machine, RunEvent::DesignPackageReady);
  apply_run(machine, RunEvent::Approved);

  NewOperation intent;
  intent.idempotency_key =
      cf::idempotency_key("R12", "", "push", cf::sha256_hex("integration-head"));
  intent.kind = "push";
  intent.input_json = "{\"branch\":\"cf/R12/integration\"}";
  intent.expected_json = "{}";
  const auto applied = machine.apply(RunEvent::AllMilestonesDone, {intent}, now_);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  auto op = store_->find_operation_by_key(intent.idempotency_key);
  ASSERT_TRUE(op.has_value()) << op.error().message;
  EXPECT_EQ(op->state, cf::store::op_state::kPending);
  EXPECT_EQ(op->kind, "push");
}

TEST_F(StateMachineTest, MilestoneHappyPath) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Pending);

  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::TestsCommitted);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesPassed);
  apply_milestone(machine, "M3", MilestoneEvent::Approved);
  apply_milestone(machine, "M3", MilestoneEvent::ExitCriteriaMet);
  apply_milestone(machine, "M3", MilestoneEvent::Merged);
  current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Done);

  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->state, "Done");
  EXPECT_EQ(stored->iterations, 1u);
}

TEST_F(StateMachineTest, MilestoneBlockedPath) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M4").has_value());
  apply_milestone(machine, "M4", MilestoneEvent::Block);
  auto current = machine.current("M4");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Blocked);
  apply_milestone(machine, "M4", MilestoneEvent::Unblock);
  current = machine.current("M4");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::TestAuthoring);
}

TEST_F(StateMachineTest, MilestoneFixLoopCountsIterations) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::TestsCommitted);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesFailed);
  auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Fixing);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesPassed);

  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->state, "Reviewing");
  EXPECT_EQ(stored->iterations, 2u);
}

TEST_F(StateMachineTest, MilestoneStallEscalatesAndResolves) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::TestsCommitted);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesFailed);
  apply_milestone(machine, "M3", MilestoneEvent::StallOrBudget);
  auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Escalated);
  apply_milestone(machine, "M3", MilestoneEvent::HumanResolved);
  current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Fixing);
}

TEST_F(StateMachineTest, AHumanWaiverReturnsAnEscalatedMilestoneToGating) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::TestsCommitted);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesFailed);
  apply_milestone(machine, "M3", MilestoneEvent::StallOrBudget);
  auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Escalated);

  apply_milestone(machine, "M3", MilestoneEvent::WaiverGranted);
  current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Gating);
}

TEST_F(StateMachineTest, MilestoneRevalidationPath) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::TestsCommitted);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesPassed);
  apply_milestone(machine, "M3", MilestoneEvent::Approved);
  apply_milestone(machine, "M3", MilestoneEvent::ExitCriteriaMet);
  apply_milestone(machine, "M3", MilestoneEvent::Merged);
  apply_milestone(machine, "M3", MilestoneEvent::InterfaceChanged);
  auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Revalidating);
  apply_milestone(machine, "M3", MilestoneEvent::GatesRerun);
  current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Gating);
}

TEST_F(StateMachineTest, GatesPassedReviewValidSkipsReview) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::TestsCommitted);
  apply_milestone(machine, "M3", MilestoneEvent::WorkSubmitted);
  apply_milestone(machine, "M3", MilestoneEvent::GatesPassedReviewValid);
  const auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::QaValidating);
}

TEST_F(StateMachineTest, StallEscalatesFromAnyActiveState) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  apply_milestone(machine, "M3", MilestoneEvent::Start);
  apply_milestone(machine, "M3", MilestoneEvent::StallOrBudget);
  const auto current = machine.current("M3");
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, MilestoneState::Escalated);
}

TEST_F(StateMachineTest, InvalidMilestoneEventIsRejected) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  const auto applied = machine.apply("M3", MilestoneEvent::GatesPassed, now_);
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, cf::orch::error_codes::kInvalidMilestoneTransition);
}

TEST_F(StateMachineTest, UnknownMilestoneIsRejected) {
  StateMachine machine(*store_, "R12");
  const auto applied = machine.apply("M9", MilestoneEvent::Start, now_);
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, cf::orch::error_codes::kMilestoneNotFound);
  const auto current = machine.current("M9");
  ASSERT_FALSE(current.has_value());
}

TEST_F(StateMachineTest, DuplicateMilestoneRegistrationFails) {
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  const auto again = machine.register_milestone("M3");
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, cf::orch::error_codes::kMilestoneExists);
}

TEST_F(StateMachineTest, StateStringRoundTrips) {
  EXPECT_EQ(cf::orch::run_state_from_string("Executing"), RunState::Executing);
  EXPECT_EQ(cf::orch::run_state_from_string("nonsense"), std::nullopt);
  EXPECT_EQ(cf::orch::milestone_state_from_string("QaValidating"),
            MilestoneState::QaValidating);
  EXPECT_EQ(cf::orch::milestone_state_from_string("nonsense"), std::nullopt);
}

}  // namespace
