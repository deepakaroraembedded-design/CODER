// Unit tests for the human approval deadlines (design section 12.1).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/orch/approval_deadline.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/store/run_store.hpp"

namespace {

using cf::orch::ApprovalDeadlines;
using cf::orch::RunEvent;
using cf::orch::RunState;
using cf::orch::StateMachine;
using cf::store::RunRecord;
using cf::store::RunStore;

constexpr cf::store::UnixTime kDay = 24 * 60 * 60;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_deadline_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class ApprovalDeadlineTest : public ::testing::Test {
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

  void create_run(const std::string& state) {
    RunRecord run;
    run.run_id = "R12";
    run.plan_id = "PLAN-X";
    run.state = state;
    run.config_json = "{}";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  cf::store::UnixTime now_ = 0;
};

TEST_F(ApprovalDeadlineTest, BusinessDaysSkipTheWeekend) {
  // 1970-01-01 was a Thursday.
  EXPECT_EQ(cf::orch::add_business_days(0, 1), kDay);          // Friday
  EXPECT_EQ(cf::orch::add_business_days(0, 2), 4 * kDay);      // Monday
  EXPECT_EQ(cf::orch::add_business_days(0, 5), 7 * kDay);      // next Thursday
  EXPECT_EQ(cf::orch::add_business_days(3 * kDay, 1), 4 * kDay);  // Sun -> Mon
}

TEST_F(ApprovalDeadlineTest, WaitsDoNotHaveADeadline) {
  create_run("Executing");
  const auto deadline = cf::orch::waiting_deadline(
      *store_, "R12", ApprovalDeadlines{});
  ASSERT_TRUE(deadline.has_value()) << deadline.error().message;
  EXPECT_FALSE(deadline->has_value());
}

TEST_F(ApprovalDeadlineTest, DesignApprovalExpiresAndAbortsTheRun) {
  create_run("Created");
  StateMachine machine(*store_, "R12");
  const cf::store::UnixTime waiting_since = now_ - 30 * kDay;
  ASSERT_TRUE(
      machine.apply(RunEvent::Start, {}, waiting_since).has_value());
  ASSERT_TRUE(
      machine.apply(RunEvent::IndexReady, {}, waiting_since).has_value());
  ASSERT_TRUE(
      machine.apply(RunEvent::DesignPackageReady, {}, waiting_since).has_value());

  const auto deadline = cf::orch::waiting_deadline(
      *store_, "R12", ApprovalDeadlines{});
  ASSERT_TRUE(deadline.has_value()) << deadline.error().message;
  ASSERT_TRUE(deadline->has_value());
  EXPECT_EQ(**deadline, cf::orch::add_business_days(waiting_since, 5));

  const auto expired = cf::orch::expire_waiting_run(
      *store_, "R12", ApprovalDeadlines{}, now_);
  ASSERT_TRUE(expired.has_value()) << expired.error().message;
  ASSERT_TRUE(expired->has_value());
  EXPECT_EQ(**expired, "approval_timeout");
  const auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, "Aborted");
}

TEST_F(ApprovalDeadlineTest, EscalationExpiresAndAbortsTheRun) {
  create_run("Executing");
  StateMachine machine(*store_, "R12");
  const cf::store::UnixTime escalated_at = now_ - 10 * kDay;
  ASSERT_TRUE(
      machine.apply(RunEvent::BudgetExceeded, {}, escalated_at).has_value());

  const auto expired = cf::orch::expire_waiting_run(
      *store_, "R12", ApprovalDeadlines{}, now_);
  ASSERT_TRUE(expired.has_value()) << expired.error().message;
  ASSERT_TRUE(expired->has_value());
  EXPECT_EQ(**expired, "escalation_timeout");
  const auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, "Aborted");
}

TEST_F(ApprovalDeadlineTest, ARunInsideItsDeadlineIsLeftAlone) {
  create_run("Created");
  StateMachine machine(*store_, "R12");
  const cf::store::UnixTime waiting_since = now_ - kDay;
  ASSERT_TRUE(
      machine.apply(RunEvent::Start, {}, waiting_since).has_value());
  ASSERT_TRUE(
      machine.apply(RunEvent::IndexReady, {}, waiting_since).has_value());
  ASSERT_TRUE(
      machine.apply(RunEvent::DesignPackageReady, {}, waiting_since).has_value());

  const auto expired = cf::orch::expire_waiting_run(
      *store_, "R12", ApprovalDeadlines{}, now_);
  ASSERT_TRUE(expired.has_value()) << expired.error().message;
  EXPECT_FALSE(expired->has_value());
  const auto run = store_->get_run("R12");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->state, "AwaitingDesignApproval");
}

TEST_F(ApprovalDeadlineTest, ConfiguredDeadlinesApply) {
  create_run("Created");
  StateMachine machine(*store_, "R12");
  const cf::store::UnixTime waiting_since = now_ - 10 * kDay;
  ASSERT_TRUE(
      machine.apply(RunEvent::Start, {}, waiting_since).has_value());
  ASSERT_TRUE(
      machine.apply(RunEvent::IndexReady, {}, waiting_since).has_value());
  ASSERT_TRUE(
      machine.apply(RunEvent::DesignPackageReady, {}, waiting_since).has_value());

  ApprovalDeadlines deadlines;
  deadlines.design_approval_business_days = 1;
  const auto expired = cf::orch::expire_waiting_run(
      *store_, "R12", deadlines, now_);
  ASSERT_TRUE(expired.has_value()) << expired.error().message;
  ASSERT_TRUE(expired->has_value());
  EXPECT_EQ(**expired, "approval_timeout");
}

}  // namespace
