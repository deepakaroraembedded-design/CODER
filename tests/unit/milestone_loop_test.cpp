// Unit tests for the milestone loop controller (design sections 7.2, 7.3, 12).
#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/orch/milestone_loop.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::FileChange;
using cf::orch::GateOutcome;
using cf::orch::LoopLimits;
using cf::orch::MilestoneDriver;
using cf::orch::MilestoneEvent;
using cf::orch::MilestoneLoop;
using cf::orch::MilestonePhase;
using cf::orch::MilestoneState;
using cf::orch::StateMachine;
using cf::orch::TurnResult;
using cf::store::RunRecord;
using cf::store::RunStore;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_loop_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class FakeDriver final : public MilestoneDriver {
 public:
  TurnResult run(MilestonePhase phase, std::string_view milestone_id) override {
    (void)milestone_id;
    phases.push_back(phase);
    if (scripts.empty()) {
      return completed_turn();
    }
    const TurnResult result = scripts.front();
    scripts.pop_front();
    return result;
  }

  GateOutcome run_gates(std::string_view milestone_id) override {
    (void)milestone_id;
    ++gate_runs;
    if (gate_scripts.empty()) {
      return gates_pass();
    }
    const GateOutcome outcome = gate_scripts.front();
    gate_scripts.pop_front();
    return outcome;
  }

  static TurnResult completed_turn() { return TurnResult{true, "ok", "", false, false, false, false, false}; }
  static TurnResult failed_turn(const std::string& detail) {
    return TurnResult{false, "", detail, false, false, false, false, false};
  }
  static TurnResult changes_requested() {
    TurnResult result = completed_turn();
    result.changes_requested = true;
    return result;
  }
  static TurnResult exit_criteria_met() {
    TurnResult result = completed_turn();
    result.exit_criteria_met = true;
    return result;
  }
  static TurnResult defects_filed() { return completed_turn(); }
  static TurnResult merged() {
    TurnResult result = completed_turn();
    result.merged = true;
    return result;
  }
  static TurnResult rebase_conflict() { return completed_turn(); }

  static GateOutcome gates_pass(bool first_gate_run = false,
                                std::vector<FileChange> changes = {},
                                int64_t passing_tests = -1) {
    GateOutcome outcome;
    outcome.passed = true;
    outcome.report.passed = true;
    outcome.changes_since_approved = std::move(changes);
    outcome.review_context.first_gate_run = first_gate_run;
    outcome.passing_tests = passing_tests;
    return outcome;
  }

  static GateOutcome gates_fail(const std::string& message, int64_t passing_tests = -1) {
    GateOutcome outcome;
    outcome.passed = false;
    outcome.report.passed = false;
    cf::gates::GateResult result;
    result.gate = "build";
    result.passed = false;
    result.findings.push_back(cf::gates::GateFinding{"build", "error", message});
    outcome.report.results.push_back(std::move(result));
    outcome.passing_tests = passing_tests;
    return outcome;
  }

  std::deque<TurnResult> scripts;
  std::deque<GateOutcome> gate_scripts;
  std::vector<MilestonePhase> phases;
  int gate_runs = 0;
};

FileChange code_change(uint32_t added = 10, uint32_t removed = 2) {
  FileChange change;
  change.path = "src/registry/registry.cpp";
  change.added_lines = added;
  change.removed_lines = removed;
  return change;
}

FileChange test_change() {
  FileChange change;
  change.path = "tests/registry_test.cpp";
  change.added_lines = 40;
  change.qa_owned = true;
  return change;
}

class MilestoneLoopTest : public ::testing::Test {
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
    run.state = "Executing";
    run.config_json = "{}";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());

    machine_ = std::make_unique<StateMachine>(*store_, "R12");
    ASSERT_TRUE(machine_->register_milestone("M3").has_value());
    ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::Start, now_).has_value());
  }

  void TearDown() override {
    machine_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  MilestoneState drive(FakeDriver& driver, LoopLimits limits = {}) {
    MilestoneLoop loop(*store_, *machine_, driver, limits);
    const auto result = loop.drive("M3", now_);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    last_reason_ = loop.last_escalation_reason();
    return result.has_value() ? *result : MilestoneState::Escalated;
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  std::unique_ptr<StateMachine> machine_;
  cf::store::UnixTime now_ = 0;
  std::string last_reason_;
};

TEST_F(MilestoneLoopTest, HappyPathReachesDone) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),      // author tests
                    FakeDriver::completed_turn(),      // code
                    FakeDriver::completed_turn(),      // review: approved
                    FakeDriver::exit_criteria_met(),   // validate
                    FakeDriver::merged()};             // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(true, {code_change()})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const std::vector<MilestonePhase> expected = {
      MilestonePhase::AuthorTests, MilestonePhase::Code, MilestonePhase::Review,
      MilestonePhase::Validate, MilestonePhase::Integrate};
  EXPECT_EQ(driver.phases, expected);
  EXPECT_EQ(driver.gate_runs, 1);

  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->state, "Done");
  EXPECT_EQ(stored->iterations, 1u);
}

TEST_F(MilestoneLoopTest, GateFailureLeadsToFixAndSecondIteration) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),     // author tests
                    FakeDriver::completed_turn(),     // code
                    FakeDriver::completed_turn(),     // fix
                    FakeDriver::completed_turn(),     // review: approved
                    FakeDriver::exit_criteria_met(),  // validate
                    FakeDriver::merged()};            // integrate
  driver.gate_scripts = {FakeDriver::gates_fail("compile error"),
                         FakeDriver::gates_pass(true, {code_change()})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const std::vector<MilestonePhase> expected = {
      MilestonePhase::AuthorTests, MilestonePhase::Code,   MilestonePhase::Fix,
      MilestonePhase::Review,      MilestonePhase::Validate, MilestonePhase::Integrate};
  EXPECT_EQ(driver.phases, expected);
  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->iterations, 2u);
}

TEST_F(MilestoneLoopTest, ChangesRequestedLeadsToFixAndDeltaReview) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),         // author tests
                    FakeDriver::completed_turn(),         // code
                    FakeDriver::changes_requested(),      // review
                    FakeDriver::completed_turn(),         // fix
                    FakeDriver::completed_turn(),         // review: approved
                    FakeDriver::exit_criteria_met(),      // validate
                    FakeDriver::merged()};                // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(true, {code_change()}),
                         FakeDriver::gates_pass(false, {code_change(5, 1)})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const std::vector<MilestonePhase> expected = {
      MilestonePhase::AuthorTests, MilestonePhase::Code,     MilestonePhase::Review,
      MilestonePhase::Fix,         MilestonePhase::Review,   MilestonePhase::Validate,
      MilestonePhase::Integrate};
  EXPECT_EQ(driver.phases, expected);
}

TEST_F(MilestoneLoopTest, DefectsFiledLeadToFix) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),     // author tests
                    FakeDriver::completed_turn(),     // code
                    FakeDriver::completed_turn(),     // review: approved
                    FakeDriver::defects_filed(),      // validate: defects
                    FakeDriver::completed_turn(),     // fix
                    FakeDriver::completed_turn(),     // review: approved
                    FakeDriver::exit_criteria_met(),  // validate
                    FakeDriver::merged()};            // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(true, {code_change()}),
                         FakeDriver::gates_pass(false, {code_change(5, 1)})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const std::vector<MilestonePhase> expected = {
      MilestonePhase::AuthorTests, MilestonePhase::Code,     MilestonePhase::Review,
      MilestonePhase::Validate,    MilestonePhase::Fix,      MilestonePhase::Review,
      MilestonePhase::Validate,    MilestonePhase::Integrate};
  EXPECT_EQ(driver.phases, expected);
}

TEST_F(MilestoneLoopTest, ReviewSkippedWhenOnlyTestsChanged) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),     // author tests
                    FakeDriver::completed_turn(),     // code
                    FakeDriver::exit_criteria_met(),  // validate
                    FakeDriver::merged()};            // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(true, {test_change()})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const std::vector<MilestonePhase> expected = {
      MilestonePhase::AuthorTests, MilestonePhase::Code, MilestonePhase::Validate,
      MilestonePhase::Integrate};
  EXPECT_EQ(driver.phases, expected);
}

TEST_F(MilestoneLoopTest, SameGateFailureThriceEscalates) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(), FakeDriver::completed_turn(),
                    FakeDriver::completed_turn(), FakeDriver::completed_turn()};
  driver.gate_scripts = {FakeDriver::gates_fail("same failure"),
                         FakeDriver::gates_fail("same failure"),
                         FakeDriver::gates_fail("same failure")};

  EXPECT_EQ(drive(driver), MilestoneState::Escalated);
  EXPECT_NE(last_reason_.find("same gate failure"), std::string::npos);
}

TEST_F(MilestoneLoopTest, IterationLimitEscalates) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),  // author tests
                    FakeDriver::completed_turn(),  // code (iteration 1)
                    FakeDriver::completed_turn()}; // fix (iteration 2)
  driver.gate_scripts = {FakeDriver::gates_fail("first failure"),
                         FakeDriver::gates_fail("second failure")};

  LoopLimits limits;
  limits.max_iterations_per_milestone = 2;
  EXPECT_EQ(drive(driver, limits), MilestoneState::Escalated);
  EXPECT_NE(last_reason_.find("iteration limit reached"), std::string::npos);
}

TEST_F(MilestoneLoopTest, TurnFailureRetriesThenEscalates) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::failed_turn("boom"), FakeDriver::failed_turn("boom"),
                    FakeDriver::failed_turn("boom")};

  EXPECT_EQ(drive(driver), MilestoneState::Escalated);
  EXPECT_NE(last_reason_.find("test authoring failed 3 times"), std::string::npos);
  EXPECT_EQ(driver.phases.size(), 3u);
}

TEST_F(MilestoneLoopTest, NoProgressEscalates) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(), FakeDriver::completed_turn(),
                    FakeDriver::completed_turn(), FakeDriver::completed_turn()};
  driver.gate_scripts = {FakeDriver::gates_fail("error one", 5),
                         FakeDriver::gates_fail("error two", 5),
                         FakeDriver::gates_fail("error three", 5)};

  LoopLimits limits;
  limits.max_no_progress_iterations = 2;
  EXPECT_EQ(drive(driver, limits), MilestoneState::Escalated);
  EXPECT_NE(last_reason_.find("no net progress"), std::string::npos);
}

TEST_F(MilestoneLoopTest, FixTurnsCountDefectAttemptsAndEscalate) {
  // Put the milestone in Fixing with one open defect.
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::TestsCommitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::WorkSubmitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::GatesFailed, now_).has_value());
  cf::store::DefectRecord defect;
  defect.id = "D-1";
  defect.run_id = "R12";
  defect.milestone_id = "M3";
  defect.status = "OPEN";
  defect.severity = "high";
  defect.body_json = "{}";
  ASSERT_TRUE(store_->upsert_defect(defect).has_value());

  // The default driver keeps the loop cycling through QA defects; each fix
  // turn counts one attempt and the fifth entry escalates the defect.
  FakeDriver driver;
  EXPECT_EQ(drive(driver), MilestoneState::Escalated);
  EXPECT_NE(last_reason_.find("D-1"), std::string::npos);
  EXPECT_NE(last_reason_.find("4 of 4 fix attempts"), std::string::npos);

  const auto stored = store_->get_defect("D-1");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->attempts, 4u);
}

TEST_F(MilestoneLoopTest, DefectAtItsLimitEscalatesImmediately) {
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::TestsCommitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::WorkSubmitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::GatesFailed, now_).has_value());
  cf::store::DefectRecord defect;
  defect.id = "D-7";
  defect.run_id = "R12";
  defect.milestone_id = "M3";
  defect.status = "REOPENED";
  defect.severity = "blocker";
  defect.attempts = 4;
  defect.body_json = "{}";
  ASSERT_TRUE(store_->upsert_defect(defect).has_value());

  FakeDriver driver;
  EXPECT_EQ(drive(driver), MilestoneState::Escalated);
  EXPECT_NE(last_reason_.find("D-7"), std::string::npos);
  EXPECT_TRUE(driver.phases.empty());  // no fix turn was attempted
}

TEST_F(MilestoneLoopTest, VerifiedDefectsDoNotCount) {
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::TestsCommitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::WorkSubmitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::GatesFailed, now_).has_value());
  cf::store::DefectRecord defect;
  defect.id = "D-2";
  defect.run_id = "R12";
  defect.milestone_id = "M3";
  defect.status = "VERIFIED";  // not open
  defect.severity = "minor";
  defect.attempts = 4;
  defect.body_json = "{}";
  ASSERT_TRUE(store_->upsert_defect(defect).has_value());

  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),      // fix
                    FakeDriver::completed_turn(),      // review: approved
                    FakeDriver::exit_criteria_met(),   // validate
                    FakeDriver::merged()};             // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(true, {code_change()}),
                         FakeDriver::gates_pass(false, {code_change(5, 1)})};
  EXPECT_EQ(drive(driver), MilestoneState::Done);
}

TEST_F(MilestoneLoopTest, StopsAtPendingAndBlocked) {
  const auto dir = make_temp_dir();
  auto opened = RunStore::open(dir / "run.db");
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<RunStore> store = std::move(*opened);
  RunRecord run;
  run.run_id = "R13";
  run.state = "Executing";
  run.created_at = now_;
  ASSERT_TRUE(store->create_run(run).has_value());
  StateMachine machine(*store, "R13");
  ASSERT_TRUE(machine.register_milestone("M1").has_value());

  FakeDriver driver;
  MilestoneLoop loop(*store, machine, driver);
  auto result = loop.drive("M1", now_);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, MilestoneState::Pending);

  ASSERT_TRUE(machine.apply("M1", MilestoneEvent::Block, now_).has_value());
  result = loop.drive("M1", now_);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, MilestoneState::Blocked);
  EXPECT_TRUE(driver.phases.empty());
  store.reset();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST_F(MilestoneLoopTest, RevalidatingRerunsGates) {
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::TestsCommitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::WorkSubmitted, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::GatesPassed, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::Approved, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::ExitCriteriaMet, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::Merged, now_).has_value());
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::InterfaceChanged, now_).has_value());

  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),     // review: approved
                    FakeDriver::exit_criteria_met(),  // validate
                    FakeDriver::merged()};            // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(false, {code_change(3, 1)})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const std::vector<MilestonePhase> expected = {MilestonePhase::Review,
                                                MilestonePhase::Validate,
                                                MilestonePhase::Integrate};
  EXPECT_EQ(driver.phases, expected);
  EXPECT_EQ(driver.gate_runs, 1);
}

TEST_F(MilestoneLoopTest, RebaseConflictReturnsToFix) {
  FakeDriver driver;
  driver.scripts = {FakeDriver::completed_turn(),     // author tests
                    FakeDriver::completed_turn(),     // code
                    FakeDriver::completed_turn(),     // review: approved
                    FakeDriver::exit_criteria_met(),  // validate
                    FakeDriver::rebase_conflict(),    // integrate: conflict
                    FakeDriver::completed_turn(),     // fix
                    FakeDriver::completed_turn(),     // review: approved
                    FakeDriver::exit_criteria_met(),  // validate
                    FakeDriver::merged()};            // integrate
  driver.gate_scripts = {FakeDriver::gates_pass(true, {code_change()}),
                         FakeDriver::gates_pass(false, {code_change(4, 1)})};

  EXPECT_EQ(drive(driver), MilestoneState::Done);
  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->iterations, 2u);
}

TEST_F(MilestoneLoopTest, EscalatedMilestoneStopsImmediately) {
  ASSERT_TRUE(machine_->apply("M3", MilestoneEvent::StallOrBudget, now_).has_value());
  FakeDriver driver;
  EXPECT_EQ(drive(driver), MilestoneState::Escalated);
  EXPECT_TRUE(driver.phases.empty());
}

}  // namespace
