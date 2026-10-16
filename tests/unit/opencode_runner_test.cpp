// Unit tests for the opencode-backed agent runner (design sections 6.6,
// 6.6.1): prompt building, submission collection, continuation messages and
// the watchdog.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/orch/opencode_runner.hpp"

namespace {

using cf::orch::AgentTurnRequest;
using cf::orch::OpencodeAgentRunner;
using cf::orch::OpencodeRunnerOptions;
using cf::orch::SessionDriver;
using Json = nlohmann::json;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_opencode_runner_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void append_call(const std::filesystem::path& path, const Json& call) {
  std::ofstream out(path, std::ios::app);
  out << call.dump() << "\n";
}

class FakeSessionDriver final : public SessionDriver {
 public:
  std::function<void(const std::string& prompt)> on_prompt;
  std::vector<std::string> prompts;
  int aborts = 0;
  bool wait_fails = false;
  // When set, the driver reports this as the finished session's usage.
  Json usage;

  cf::Result<std::string> create_session() override {
    return std::string("ses_test");
  }

  cf::Status send_prompt(std::string_view session_id, std::string_view text) override {
    (void)session_id;
    prompts.push_back(std::string(text));
    if (on_prompt) {
      on_prompt(prompts.back());
    }
    return {};
  }

  cf::Status wait_idle(std::string_view session_id,
                       std::chrono::milliseconds timeout) override {
    (void)session_id;
    (void)timeout;
    if (wait_fails) {
      return cf::fail("OC_TIMEOUT", "no events for 120 s");
    }
    return {};
  }

  cf::Status abort(std::string_view session_id) override {
    (void)session_id;
    ++aborts;
    return {};
  }

  cf::Result<Json> session_usage(std::string_view session_id) override {
    (void)session_id;
    if (usage.is_null()) {
      return cf::fail("ORCH_USAGE_UNAVAILABLE", "no usage in the fake");
    }
    return usage;
  }
};

class OpencodeRunnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    calls_file_ = dir_ / "calls.jsonl";
    options_.calls_file = calls_file_.string();
    options_.max_continuations = 2;
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  AgentTurnRequest request(cf::orch::MilestonePhase phase,
                           const std::string& role = "coder") const {
    AgentTurnRequest request;
    request.phase = phase;
    request.run_id = "R12";
    request.milestone_id = "M3";
    request.role = role;
    request.worktree = "/work";
    request.requirement_ids = {"REQ-010"};
    request.brief = "Implement the registry";
    return request;
  }

  std::filesystem::path dir_;
  std::filesystem::path calls_file_;
  OpencodeRunnerOptions options_;
};

TEST_F(OpencodeRunnerTest, WorkSubmissionEndsTheTurn) {
  FakeSessionDriver driver;
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_, Json{{"tool", "submit_work"},
                                  {"arguments",
                                   Json{{"summary", "did it"},
                                        {"defect_ids", Json::array()}}}});
  };
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_TRUE(outcome->completed);
  EXPECT_EQ(outcome->summary, "did it");
  EXPECT_EQ(driver.prompts.size(), 1u);
}

TEST_F(OpencodeRunnerTest, ContinuationIsSentWhenNoSubmissionArrives) {
  FakeSessionDriver driver;
  int prompts = 0;
  driver.on_prompt = [this, &prompts](const std::string&) {
    ++prompts;
    if (prompts == 2) {
      append_call(calls_file_, Json{{"tool", "submit_work"},
                                    {"arguments", Json{{"summary", "second try"}}}});
    }
  };
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_TRUE(outcome->completed);
  EXPECT_EQ(driver.prompts.size(), 2u);
  EXPECT_EQ(driver.prompts[1], options_.continuation_message);
  EXPECT_NE(driver.prompts[0].find("Implement the registry"), std::string::npos);
}

TEST_F(OpencodeRunnerTest, GivesUpAfterTheContinuationLimit) {
  FakeSessionDriver driver;  // never submits
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().code, "ORCH_NO_SUBMISSION");
  EXPECT_EQ(driver.prompts.size(), 3u);  // 1 prompt + 2 continuations
}

TEST_F(OpencodeRunnerTest, WatchdogTimeoutAbortsTheSession) {
  FakeSessionDriver driver;
  driver.wait_fails = true;
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().code, "ORCH_TURN_TIMEOUT");
  EXPECT_EQ(driver.aborts, 1);
}

TEST_F(OpencodeRunnerTest, ReviewVerdictMapsToChangesRequested) {
  FakeSessionDriver driver;
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_,
                Json{{"tool", "submit_review"},
                     {"arguments",
                      Json{{"verdict_json",
                            Json{{"verdict", "CHANGES_REQUESTED"},
                                 {"summary", "factory not used"}}}}}});
  };
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Review, "reviewer"));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_TRUE(outcome->completed);
  EXPECT_TRUE(outcome->changes_requested);
  EXPECT_EQ(outcome->summary, "factory not used");
}

TEST_F(OpencodeRunnerTest, FiledDefectFailsExitCriteria) {
  FakeSessionDriver driver;
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_,
                Json{{"tool", "file_defect"},
                     {"arguments",
                      Json{{"defect_json",
                            Json{{"summary", "duplicate ids"},
                                 {"severity", "high"}}}}}});
  };
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Validate, "qa"));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_TRUE(outcome->completed);
  EXPECT_FALSE(outcome->exit_criteria_met);
}

TEST_F(OpencodeRunnerTest, VerifiedDefectsMeetExitCriteria) {
  FakeSessionDriver driver;
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_, Json{{"tool", "verify_defect"},
                                  {"arguments",
                                   Json{{"id", "D-1"}, {"result", "VERIFIED"}}}});
  };
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Validate, "qa"));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_TRUE(outcome->completed);
  EXPECT_TRUE(outcome->exit_criteria_met);
}

TEST_F(OpencodeRunnerTest, SystemValidationUsesTheValidateAcceptance) {
  FakeSessionDriver driver;
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_,
                Json{{"tool", "submit_tests"},
                     {"arguments",
                      Json{{"plan_ref", "qa/plan/system.yaml"}, {"summary", "all pass"}}}});
  };
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_system_validation(
      request(cf::orch::MilestonePhase::Validate, "qa"));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_TRUE(outcome->completed);
  EXPECT_TRUE(outcome->exit_criteria_met);
  EXPECT_NE(driver.prompts[0].find("system validation"), std::string::npos);
}

TEST_F(OpencodeRunnerTest, OldCallsAreNotReused) {
  append_call(calls_file_, Json{{"tool", "submit_work"},
                                {"arguments", Json{{"summary", "stale"}}}});
  FakeSessionDriver driver;  // no new calls this turn
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().code, "ORCH_NO_SUBMISSION");
}

TEST_F(OpencodeRunnerTest, RecordsSessionUsageAfterASuccessfulTurn) {
  FakeSessionDriver driver;
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_, Json{{"tool", "submit_work"},
                                  {"arguments", Json{{"summary", "done"}}}});
  };
  driver.usage = Json{{"cost", 0.25}, {"tokens", Json{{"input", 100}, {"output", 40}}}};
  options_.usage_file = dir_ / "usage-coder.jsonl";
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;

  std::ifstream in(options_.usage_file);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("\"role\":\"coder\""), std::string::npos);
  EXPECT_NE(text.find("\"cost\":0.25"), std::string::npos);
  EXPECT_NE(text.find("\"session_id\":\"ses_test\""), std::string::npos);
}

TEST_F(OpencodeRunnerTest, UsageIsSkippedWhenTheDriverCannotReportIt) {
  FakeSessionDriver driver;  // default session_usage: unavailable
  driver.on_prompt = [this](const std::string&) {
    append_call(calls_file_, Json{{"tool", "submit_work"},
                                  {"arguments", Json{{"summary", "done"}}}});
  };
  options_.usage_file = dir_ / "usage-coder.jsonl";
  OpencodeAgentRunner runner(driver, options_);
  auto outcome = runner.run_turn(request(cf::orch::MilestonePhase::Code));
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;
  EXPECT_FALSE(std::filesystem::exists(options_.usage_file));
}

TEST_F(OpencodeRunnerTest, PromptCarriesRoleLanguageAndRequirements) {
  FakeSessionDriver driver;
  OpencodeAgentRunner runner(driver, options_);
  const std::string prompt =
      runner.build_prompt(request(cf::orch::MilestonePhase::Code), false);
  EXPECT_NE(prompt.find("Coder agent"), std::string::npos);
  EXPECT_NE(prompt.find("cpp"), std::string::npos);
  EXPECT_NE(prompt.find("REQ-010"), std::string::npos);
  EXPECT_NE(prompt.find("Implement the registry"), std::string::npos);
  EXPECT_NE(prompt.find("submit_work"), std::string::npos);
  EXPECT_NE(prompt.find("Do not ask questions"), std::string::npos);
}

}  // namespace
