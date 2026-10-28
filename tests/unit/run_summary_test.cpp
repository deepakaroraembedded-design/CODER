// Unit tests for the shared run summary: agent-turn usage and totals
// (design sections 6.11, 14).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/orch/run_summary.hpp"
#include "cf/store/run_store.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_summary_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

class RunSummaryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = cf::store::RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.plan_id = "PLAN-X";
    run.state = "Executing";
    run.config_json = "{}";
    run.created_at = cf::store::unix_now();
    ASSERT_TRUE(store_->create_run(run).has_value());
  }

  void TearDown() override {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::store::RunStore> store_;
};

TEST_F(RunSummaryTest, SumsAgentUsageByRoleAndMilestone) {
  write_file(dir_ / "usage-coder.jsonl",
             R"({"role":"coder","milestone_id":"M1","usage":{"cost":0.5,"tokens":{"input":100,"output":50,"cache":{"read":20}}}})"
             "\n");
  write_file(dir_ / "usage-qa.jsonl",
             R"({"role":"qa","milestone_id":"M2","usage":{"cost":0.25,"tokens":{"prompt":40,"completion":10,"reasoning":5}}})"
             "\n");

  auto summary = cf::orch::build_run_summary(*store_, "R12", nullptr, dir_);
  ASSERT_TRUE(summary.has_value()) << summary.error().message;
  EXPECT_DOUBLE_EQ(summary->usd, 0.75);
  EXPECT_EQ(summary->tokens, 205);  // 150 coder + 55 qa

  ASSERT_EQ(summary->agent_usage_by_role.size(), 2u);
  const auto& coder = summary->agent_usage_by_role[0];
  EXPECT_EQ(coder["role"], "coder");
  EXPECT_DOUBLE_EQ(coder["usd"].get<double>(), 0.5);
  EXPECT_EQ(coder["prompt_tokens"], 100);
  EXPECT_EQ(coder["completion_tokens"], 50);
  EXPECT_EQ(coder["cached_tokens"], 20);
  EXPECT_EQ(coder["tokens"], 150);
  const auto& qa = summary->agent_usage_by_role[1];
  EXPECT_EQ(qa["role"], "qa");
  EXPECT_EQ(qa["tokens"], 55);

  ASSERT_EQ(summary->agent_usage_by_milestone.size(), 2u);
  EXPECT_EQ(summary->agent_usage_by_milestone[0]["milestone_id"], "M1");
  EXPECT_EQ(summary->agent_usage_by_milestone[1]["milestone_id"], "M2");

  const std::string markdown =
      cf::orch::render_run_summary_markdown(*summary);
  EXPECT_NE(markdown.find("## Agent usage"), std::string::npos);
  EXPECT_NE(markdown.find("| coder |"), std::string::npos);
  EXPECT_NE(markdown.find("| M2 |"), std::string::npos);
}

TEST_F(RunSummaryTest, AddsTheStoreLedgerToTheAgentUsage) {
  cf::store::LlmCallRecord call;
  call.run_id = "R12";
  call.role = "planner";
  call.model = "planner-model";
  call.prompt_tokens = 7;
  call.completion_tokens = 3;
  call.usd = 1.5;
  call.at = cf::store::unix_now();
  ASSERT_TRUE(store_->record_llm_call(call).has_value());

  write_file(dir_ / "usage-coder.jsonl",
             R"({"role":"coder","milestone_id":"M1","usage":{"cost":0.5,"tokens":{"input":10,"output":5}}})"
             "\n");

  auto summary = cf::orch::build_run_summary(*store_, "R12", nullptr, dir_);
  ASSERT_TRUE(summary.has_value()) << summary.error().message;
  EXPECT_DOUBLE_EQ(summary->usd, 2.0);
  EXPECT_EQ(summary->tokens, 25);  // 10 ledger + 15 coder
}

TEST_F(RunSummaryTest, MalformedUsageLinesAreIgnored) {
  write_file(dir_ / "usage-coder.jsonl",
             "not json\n"
             R"({"role":"coder","milestone_id":"M1","usage":{"tokens":{"input":4,"output":2}}})"
             "\n");
  auto summary = cf::orch::build_run_summary(*store_, "R12", nullptr, dir_);
  ASSERT_TRUE(summary.has_value()) << summary.error().message;
  EXPECT_EQ(summary->tokens, 6);
  ASSERT_EQ(summary->agent_usage_by_role.size(), 1u);
}

}  // namespace
