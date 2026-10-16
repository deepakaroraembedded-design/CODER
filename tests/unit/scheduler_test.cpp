// Unit tests for the dependency-aware milestone scheduler (design section
// 6.13.2).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/orch/scheduler.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::MilestoneEvent;
using cf::orch::MilestoneState;
using cf::orch::Scheduler;
using cf::orch::SchedulerConfig;
using cf::orch::StateMachine;
using cf::plan::MilestoneSpec;
using cf::plan::Plan;
using cf::store::RunRecord;
using cf::store::RunStore;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_sched_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

MilestoneSpec milestone(const std::string& id,
                        const std::vector<std::string>& depends_on = {}) {
  MilestoneSpec spec;
  spec.id = id;
  spec.title = id;
  spec.depends_on = depends_on;
  return spec;
}

class SchedulerTest : public ::testing::Test {
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

  void register_all(StateMachine& machine, const Plan& plan) {
    for (const auto& spec : plan.milestones) {
      ASSERT_TRUE(machine.register_milestone(spec.id).has_value());
    }
  }

  void apply(StateMachine& machine, const std::string& id, MilestoneEvent event) {
    const auto applied = machine.apply(id, event, now_);
    ASSERT_TRUE(applied.has_value()) << applied.error().message;
  }

  void make_active(StateMachine& machine, const std::string& id) {
    apply(machine, id, MilestoneEvent::Start);
  }

  void make_done(StateMachine& machine, const std::string& id) {
    const auto current = machine.current(id);
    ASSERT_TRUE(current.has_value());
    if (*current == MilestoneState::Pending) {
      apply(machine, id, MilestoneEvent::Start);
    }
    apply(machine, id, MilestoneEvent::TestsCommitted);
    apply(machine, id, MilestoneEvent::WorkSubmitted);
    apply(machine, id, MilestoneEvent::GatesPassed);
    apply(machine, id, MilestoneEvent::Approved);
    apply(machine, id, MilestoneEvent::ExitCriteriaMet);
    apply(machine, id, MilestoneEvent::Merged);
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  cf::store::UnixTime now_ = 0;
};

TEST_F(SchedulerTest, IndependentMilestonesStartUpToCap) {
  Plan plan = make_plan({milestone("M1"), milestone("M2"), milestone("M3")});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{2});
  auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value()) << decision.error().message;
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1", "M2"}));
  EXPECT_TRUE(decision->unblock.empty());
  EXPECT_TRUE(decision->block.empty());

  make_active(machine, "M1");
  make_done(machine, "M2");
  decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M3"}));
}

TEST_F(SchedulerTest, DependencyMustBeDoneBeforeStart) {
  Plan plan = make_plan({milestone("M1"), milestone("M2", {"M1"})});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{4});
  auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1"}));

  make_done(machine, "M1");
  decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M2"}));
}

TEST_F(SchedulerTest, DependencyInRevalidatingIsNotDone) {
  Plan plan = make_plan({milestone("M1"), milestone("M2", {"M1"})});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);
  make_done(machine, "M1");
  apply(machine, "M1", MilestoneEvent::InterfaceChanged);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{4});
  const auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_TRUE(decision->start.empty());
}

TEST_F(SchedulerTest, WriteOverlapWaitsWithoutBlocking) {
  MilestoneSpec m1 = milestone("M1");
  m1.writes = {"src/a.cpp"};
  MilestoneSpec m2 = milestone("M2");
  m2.writes = {"src/a.cpp", "src/b.cpp"};
  Plan plan = make_plan({m1, m2});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{4});
  auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1"}));
  EXPECT_TRUE(decision->block.empty());

  make_done(machine, "M1");
  decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M2"}));
}

TEST_F(SchedulerTest, InterfaceWriteBlocksUser) {
  MilestoneSpec m1 = milestone("M1");
  m1.writes_interfaces = {"IFoo"};
  MilestoneSpec m2 = milestone("M2");
  m2.uses = {"IFoo"};
  Plan plan = make_plan({m1, m2});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{4});
  auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1"}));
  EXPECT_EQ(decision->block, (std::vector<std::string>{"M2"}));

  // The orchestrator applies the Block transition; the milestone waits.
  apply(machine, "M2", MilestoneEvent::Block);
  const auto state = machine.current("M2");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(*state, MilestoneState::Blocked);

  // Once the writer is merged, the user unblocks.
  make_done(machine, "M1");
  decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->unblock, (std::vector<std::string>{"M2"}));
  EXPECT_TRUE(decision->block.empty());
}

TEST_F(SchedulerTest, InterfaceUseBlocksWriterToo) {
  MilestoneSpec m1 = milestone("M1");
  m1.uses = {"IFoo"};
  MilestoneSpec m2 = milestone("M2");
  m2.writes_interfaces = {"IFoo"};
  Plan plan = make_plan({m1, m2});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{4});
  const auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1"}));
  EXPECT_EQ(decision->block, (std::vector<std::string>{"M2"}));
}

TEST_F(SchedulerTest, BlockedMilestoneDoesNotConsumeCapacity) {
  MilestoneSpec m1 = milestone("M1");
  m1.writes_interfaces = {"IFoo"};
  MilestoneSpec m2 = milestone("M2");
  m2.uses = {"IFoo"};
  MilestoneSpec m3 = milestone("M3");
  Plan plan = make_plan({m1, m2, m3});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{2});
  auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  // M1 occupies the single slot that matters; M2 is blocked, not active, so
  // M3 may still start alongside M1.
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1", "M3"}));
  EXPECT_EQ(decision->block, (std::vector<std::string>{"M2"}));
}

TEST_F(SchedulerTest, CapOneSerializesIndependentWork) {
  Plan plan = make_plan({milestone("M1"), milestone("M2")});
  StateMachine machine(*store_, "R12");
  register_all(machine, plan);

  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{1});
  auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1"}));

  make_active(machine, "M1");
  decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_TRUE(decision->start.empty());

  make_done(machine, "M1");
  decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M2"}));
}

TEST_F(SchedulerTest, UnregisteredMilestonesArePending) {
  Plan plan = make_plan({milestone("M1"), milestone("M2")});
  Scheduler scheduler(*store_, "R12", plan, SchedulerConfig{1});
  const auto decision = scheduler.next();
  ASSERT_TRUE(decision.has_value());
  EXPECT_EQ(decision->start, (std::vector<std::string>{"M1"}));
}

TEST_F(SchedulerTest, IsActiveClassification) {
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::TestAuthoring));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::Coding));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::Gating));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::Reviewing));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::QaValidating));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::Fixing));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::Integrating));
  EXPECT_TRUE(cf::orch::is_active(MilestoneState::Revalidating));
  EXPECT_FALSE(cf::orch::is_active(MilestoneState::Pending));
  EXPECT_FALSE(cf::orch::is_active(MilestoneState::Blocked));
  EXPECT_FALSE(cf::orch::is_active(MilestoneState::Done));
  EXPECT_FALSE(cf::orch::is_active(MilestoneState::Escalated));
}

}  // namespace
