// Unit tests for the run-level orchestrator (design sections 6.1, 7.1, 12).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/orch/orchestrator.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::GateOutcome;
using cf::orch::MilestoneDriver;
using cf::orch::MilestoneEvent;
using cf::orch::MilestonePhase;
using cf::orch::MilestoneState;
using cf::orch::OrchestratorConfig;
using cf::orch::RunOrchestrator;
using cf::orch::RunState;
using cf::orch::StateMachine;
using cf::orch::SystemValidationResult;
using cf::orch::TurnResult;
using cf::plan::MilestoneSpec;
using cf::plan::Plan;
using cf::store::LlmCallRecord;
using cf::store::RunRecord;
using cf::store::RunStore;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_orch_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class FakeDriver final : public MilestoneDriver {
 public:
  std::function<TurnResult(MilestonePhase, const std::string&)> on_turn;
  std::function<GateOutcome(const std::string&)> on_gates;
  SystemValidationResult system_result;
  std::vector<std::string> log;

  TurnResult run(MilestonePhase phase, std::string_view milestone_id) override {
    const std::string id(milestone_id);
    log.push_back(std::to_string(static_cast<int>(phase)) + ":" + id);
    if (on_turn) {
      return on_turn(phase, id);
    }
    return TurnResult{true, "ok", "", false, false, false, false, false, ""};
  }

  GateOutcome run_gates(std::string_view milestone_id) override {
    log.push_back("gates:" + std::string(milestone_id));
    if (on_gates) {
      return on_gates(std::string(milestone_id));
    }
    GateOutcome outcome;
    outcome.passed = true;
    outcome.report.passed = true;
    return outcome;
  }

  std::vector<std::string> integration_interfaces(std::string_view milestone_id) override {
    const auto it = interfaces.find(std::string(milestone_id));
    return it == interfaces.end() ? std::vector<std::string>{} : it->second;
  }

  void mark_revalidating(std::string_view milestone_id) override {
    revalidated.push_back(std::string(milestone_id));
  }

  SystemValidationResult validate_system() override { return system_result; }

  std::map<std::string, std::vector<std::string>, std::less<>> interfaces;
  std::vector<std::string> revalidated;
};

TurnResult successful_turn(MilestonePhase phase, const std::string& milestone_id) {
  (void)milestone_id;
  TurnResult result{true, "ok", "", false, false, false, false, false, ""};
  if (phase == MilestonePhase::Validate) {
    result.exit_criteria_met = true;
  }
  if (phase == MilestonePhase::Integrate) {
    result.merged = true;
  }
  return result;
}

GateOutcome full_review_gates(const std::string& milestone_id) {
  (void)milestone_id;
  GateOutcome outcome;
  outcome.passed = true;
  outcome.report.passed = true;
  outcome.review_context.first_gate_run = true;
  cf::orch::FileChange change;
  change.path = "src/registry/registry.cpp";
  change.added_lines = 10;
  change.removed_lines = 2;
  outcome.changes_since_approved.push_back(change);
  return outcome;
}

MilestoneSpec milestone(const std::string& id,
                        const std::vector<std::string>& depends_on = {}) {
  MilestoneSpec spec;
  spec.id = id;
  spec.title = id;
  spec.depends_on = depends_on;
  return spec;
}

class OrchestratorTest : public ::testing::Test {
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

  void create_run(const std::string& run_id = "R12",
                  const std::string& state = "Executing") {
    RunRecord run;
    run.run_id = run_id;
    run.plan_id = "PLAN-X";
    run.state = state;
    run.config_json = "{}";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());
  }

  Plan make_plan(std::vector<MilestoneSpec> milestones) {
    Plan plan;
    plan.plan_id = "PLAN-X";
    plan.product = "Y";
    plan.version = "1";
    plan.milestones = std::move(milestones);
    plan.target.language = "cpp";
    plan.target.standard = "c++20";
    plan.target.build_system = "cmake";
    plan.target.test_framework = "gtest";
    return plan;
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  cf::store::UnixTime now_ = 0;
};

TEST_F(OrchestratorTest, RunReachesDoneThroughMilestones) {
  create_run();
  Plan plan = make_plan({milestone("M1"), milestone("M2", {"M1"})});
  FakeDriver driver;
  driver.on_turn = successful_turn;
  driver.on_gates = full_review_gates;
  driver.system_result = SystemValidationResult{true, true, ""};

  RunOrchestrator orchestrator(*store_, "R12", plan, driver);

  auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::SystemValidation);

  const auto m1 = store_->get_milestone("R12", "M1");
  const auto m2 = store_->get_milestone("R12", "M2");
  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m1->state, "Done");
  EXPECT_EQ(m2->state, "Done");

  state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Done);
}

TEST_F(OrchestratorTest, AChangedInterfaceRevalidatesDoneDependents) {
  create_run();
  MilestoneSpec api = milestone("M2", {"M1"});
  api.uses = {"include/core.hpp"};
  Plan plan = make_plan({milestone("M1"), api, milestone("M3", {"M2"})});

  FakeDriver driver;
  driver.on_turn = successful_turn;
  driver.on_gates = full_review_gates;
  // M3's merge changes the interface M2 uses.
  driver.interfaces["M3"] = {"include/core.hpp"};
  driver.system_result = SystemValidationResult{true, true, ""};

  RunOrchestrator orchestrator(*store_, "R12", plan, driver);
  auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::SystemValidation);

  // M2 was Done when M3 changed the interface it uses: it revalidated (gates
  // ran again) before the run could finish (design section 6.13.3).
  EXPECT_EQ(driver.revalidated, (std::vector<std::string>{"M2"}));
  int m2_gates = 0;
  for (const auto& entry : driver.log) {
    if (entry == "gates:M2") {
      ++m2_gates;
    }
  }
  EXPECT_EQ(m2_gates, 2);
  const auto m2 = store_->get_milestone("R12", "M2");
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m2->state, "Done");
}

TEST_F(OrchestratorTest, IndependentMilestonesBothComplete) {
  create_run();
  Plan plan = make_plan({milestone("M1"), milestone("M2")});
  FakeDriver driver;
  driver.on_turn = successful_turn;
  driver.on_gates = full_review_gates;
  driver.system_result = SystemValidationResult{true, true, ""};

  RunOrchestrator orchestrator(*store_, "R12", plan, driver);
  auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::SystemValidation);

  const auto m1 = store_->get_milestone("R12", "M1");
  const auto m2 = store_->get_milestone("R12", "M2");
  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m1->state, "Done");
  EXPECT_EQ(m2->state, "Done");
}

TEST_F(OrchestratorTest, SystemValidationFailureReturnsToExecuting) {
  create_run();
  Plan plan = make_plan({milestone("M1")});
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M1").has_value());
  for (const MilestoneEvent event :
       {MilestoneEvent::Start, MilestoneEvent::TestsCommitted,
        MilestoneEvent::WorkSubmitted, MilestoneEvent::GatesPassed,
        MilestoneEvent::Approved, MilestoneEvent::ExitCriteriaMet,
        MilestoneEvent::Merged}) {
    ASSERT_TRUE(machine.apply("M1", event, now_).has_value());
  }
  ASSERT_TRUE(machine
                  .apply(cf::orch::RunEvent::AllMilestonesDone, {}, now_)
                  .has_value());

  FakeDriver driver;
  driver.system_result = SystemValidationResult{true, false, "system defects"};
  RunOrchestrator orchestrator(*store_, "R12", plan, driver);
  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Executing);
}

TEST_F(OrchestratorTest, BudgetExceededEscalatesAndWritesBundle) {
  create_run();
  Plan plan = make_plan({milestone("M1")});

  LlmCallRecord call;
  call.run_id = "R12";
  call.role = "coder";
  call.model = "test-model";
  call.usd = 2.0;
  call.prompt_tokens = 100;
  call.at = now_;
  ASSERT_TRUE(store_->record_llm_call(call).has_value());

  FakeDriver driver;
  OrchestratorConfig config;
  config.max_usd_total = 1.0;
  config.escalation_dir = (dir_ / "escalations").string();
  RunOrchestrator orchestrator(*store_, "R12", plan, driver, config);

  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Escalated);
  EXPECT_NE(orchestrator.last_escalation_reason().find("budget"), std::string::npos);
  EXPECT_TRUE(driver.log.empty());  // escalation happens before any work

  const auto bundle_path = dir_ / "escalations" / ("esc-" + std::to_string(now_)) /
                           "bundle.json";
  ASSERT_TRUE(std::filesystem::exists(bundle_path));
  std::ifstream in(bundle_path);
  const nlohmann::json bundle = nlohmann::json::parse(in);
  EXPECT_EQ(bundle["run_id"], "R12");
  EXPECT_EQ(bundle["run_state"], "Escalated");
  EXPECT_NE(bundle["reason"].get<std::string>().find("budget"), std::string::npos);
  ASSERT_EQ(bundle["milestones"].size(), 1u);
  EXPECT_EQ(bundle["milestones"][0]["id"], "M1");
  EXPECT_DOUBLE_EQ(bundle["cost"]["usd"].get<double>(), 2.0);
}

TEST_F(OrchestratorTest, WallClockBudgetExceededEscalates) {
  create_run("R12", "Created");
  Plan plan = make_plan({milestone("M1")});

  StateMachine machine(*store_, "R12");
  const cf::store::UnixTime two_hours_ago = now_ - 2 * 3600;
  const cf::store::UnixTime one_hour_ago = now_ - 3600;
  ASSERT_TRUE(machine.apply(cf::orch::RunEvent::Start, {}, two_hours_ago).has_value());
  ASSERT_TRUE(
      machine.apply(cf::orch::RunEvent::IndexReady, {}, two_hours_ago).has_value());
  ASSERT_TRUE(machine
                  .apply(cf::orch::RunEvent::DesignPackageReady, {}, one_hour_ago)
                  .has_value());
  ASSERT_TRUE(
      machine.apply(cf::orch::RunEvent::Approved, {}, one_hour_ago).has_value());

  FakeDriver driver;
  OrchestratorConfig config;
  config.max_wall_clock_hours = 1.5;  // 2 active hours so far
  RunOrchestrator orchestrator(*store_, "R12", plan, driver, config);

  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Escalated);
  EXPECT_NE(orchestrator.last_escalation_reason().find("wall-clock budget"),
            std::string::npos);
  EXPECT_TRUE(driver.log.empty());  // escalation happens before any work
}

TEST_F(OrchestratorTest, WaitingForPeopleDoesNotConsumeTheWallClockBudget) {
  create_run("R12", "Created");
  Plan plan = make_plan({milestone("M1")});

  StateMachine machine(*store_, "R12");
  const cf::store::UnixTime eleven_hours_ago = now_ - 11 * 3600;
  const cf::store::UnixTime ten_hours_ago = now_ - 10 * 3600;
  const cf::store::UnixTime one_hour_ago = now_ - 3600;
  ASSERT_TRUE(
      machine.apply(cf::orch::RunEvent::Start, {}, eleven_hours_ago).has_value());
  ASSERT_TRUE(
      machine.apply(cf::orch::RunEvent::IndexReady, {}, ten_hours_ago).has_value());
  ASSERT_TRUE(machine
                  .apply(cf::orch::RunEvent::DesignPackageReady, {}, ten_hours_ago)
                  .has_value());
  ASSERT_TRUE(
      machine.apply(cf::orch::RunEvent::Approved, {}, one_hour_ago).has_value());

  FakeDriver driver;
  driver.on_turn = successful_turn;
  driver.on_gates = full_review_gates;
  OrchestratorConfig config;
  // 2 active hours (1 ingesting/refining + 1 executing); the 9 hours spent
  // awaiting design approval do not count (design sections 12.1, 15).
  config.max_wall_clock_hours = 3.0;
  RunOrchestrator orchestrator(*store_, "R12", plan, driver, config);

  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::SystemValidation);
  EXPECT_FALSE(driver.log.empty());
}

TEST_F(OrchestratorTest, StuckRunEscalatesWithNoProgress) {
  create_run();
  Plan plan = make_plan({milestone("M1"), milestone("M2", {"M1"})});
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M1").has_value());
  ASSERT_TRUE(machine.apply("M1", MilestoneEvent::Start, now_).has_value());
  ASSERT_TRUE(machine.apply("M1", MilestoneEvent::StallOrBudget, now_).has_value());

  FakeDriver driver;
  RunOrchestrator orchestrator(*store_, "R12", plan, driver);
  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Escalated);
  EXPECT_NE(orchestrator.last_escalation_reason().find("no milestone can progress"),
            std::string::npos);
}

TEST_F(OrchestratorTest, EscalatedMilestoneDoesNotBlockOthers) {
  create_run();
  Plan plan = make_plan({milestone("M1"), milestone("M2")});
  FakeDriver driver;
  driver.on_turn = [](MilestonePhase phase, const std::string& milestone_id) {
    if (milestone_id == "M1") {
      return TurnResult{false, "", "always fails", false, false, false, false, false, ""};
    }
    return successful_turn(phase, milestone_id);
  };
  driver.on_gates = full_review_gates;

  RunOrchestrator orchestrator(*store_, "R12", plan, driver);
  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  // M2 completed; M1 escalated; nothing can progress any more.
  EXPECT_EQ(*state, RunState::Escalated);
  const auto m1 = store_->get_milestone("R12", "M1");
  const auto m2 = store_->get_milestone("R12", "M2");
  ASSERT_TRUE(m1.has_value());
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m1->state, "Escalated");
  EXPECT_EQ(m2->state, "Done");
}

TEST_F(OrchestratorTest, WaitingRunStatesAreUnchanged) {
  create_run("R12", "Created");
  Plan plan = make_plan({milestone("M1")});
  FakeDriver driver;
  RunOrchestrator orchestrator(*store_, "R12", plan, driver);
  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(*state, RunState::Created);
  EXPECT_TRUE(driver.log.empty());
}

}  // namespace
