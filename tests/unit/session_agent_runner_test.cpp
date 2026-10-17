// Unit tests for the per-(role, worktree) agent runner cache (design section
// 6.12, scenario H8): a dead opencode server is detected and recreated before
// the next turn instead of failing every retry.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "cf/orch/session_agent_runner.hpp"

namespace {

using cf::orch::AgentTurnRequest;
using cf::orch::MilestonePhase;
using cf::orch::OpencodeRunnerOptions;
using cf::orch::SessionAgentRunner;
using cf::orch::SessionDriver;

class FakeDriver final : public SessionDriver {
 public:
  explicit FakeDriver(bool* healthy) : healthy_(healthy) {}

  cf::Result<std::string> create_session() override { return std::string("ses_1"); }
  cf::Status send_prompt(std::string_view, std::string_view) override { return {}; }
  cf::Status wait_idle(std::string_view, std::chrono::milliseconds,
                       std::chrono::milliseconds) override {
    return {};
  }
  cf::Status abort(std::string_view) override { return {}; }
  bool healthy() const override { return *healthy_; }

 private:
  bool* healthy_;
};

TEST(SessionAgentRunnerTest, RecreatesTheDriverWhenTheServerDied) {
  bool healthy = true;
  int factories = 0;
  SessionAgentRunner runner(
      [&](const std::string&, const std::string&)
          -> cf::Result<std::unique_ptr<SessionDriver>> {
        ++factories;
        return std::unique_ptr<SessionDriver>(std::make_unique<FakeDriver>(&healthy));
      },
      "/tmp/cf-calls", OpencodeRunnerOptions{});

  AgentTurnRequest request;
  request.phase = MilestonePhase::Code;
  request.run_id = "R12";
  request.milestone_id = "M3";
  request.role = "coder";
  request.worktree = "/work";

  // The fake never submits, so the turn fails, but the cache is what matters.
  (void)runner.run_turn(request);
  EXPECT_EQ(factories, 1);

  healthy = false;
  (void)runner.run_turn(request);
  EXPECT_EQ(factories, 2);  // the dead server was replaced

  healthy = true;  // the fresh server answers
  (void)runner.run_turn(request);
  EXPECT_EQ(factories, 2);  // and is reused
}

TEST(SessionAgentRunnerTest, KeepsOneRunnerPerRoleAndWorktree) {
  bool healthy = true;
  int factories = 0;
  SessionAgentRunner runner(
      [&](const std::string&, const std::string&)
          -> cf::Result<std::unique_ptr<SessionDriver>> {
        ++factories;
        return std::unique_ptr<SessionDriver>(std::make_unique<FakeDriver>(&healthy));
      },
      "/tmp/cf-calls", OpencodeRunnerOptions{});

  AgentTurnRequest coder;
  coder.phase = MilestonePhase::Code;
  coder.role = "coder";
  coder.worktree = "/work/m1";
  (void)runner.run_turn(coder);

  AgentTurnRequest qa = coder;
  qa.role = "qa";
  (void)runner.run_turn(qa);

  AgentTurnRequest coder_again = coder;
  (void)runner.run_turn(coder_again);
  EXPECT_EQ(factories, 2);  // one driver per (role, worktree)
}

}  // namespace
