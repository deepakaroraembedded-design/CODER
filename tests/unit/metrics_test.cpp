// Unit tests for the run metrics snapshot (design section 14).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <unistd.h>

#include "cf/orch/metrics.hpp"
#include "cf/store/run_store.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_metrics_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

class MetricsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = cf::store::RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    now_ = cf::store::unix_now();
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Executing";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());
  }

  void TearDown() override {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  static double sample(const std::vector<cf::orch::MetricSample>& samples,
                       const std::string& name,
                       const std::string& label_key,
                       const std::string& label_value) {
    for (const auto& entry : samples) {
      if (entry.name != name) {
        continue;
      }
      if (label_key.empty()) {
        return entry.value;
      }
      const auto label = entry.labels.find(label_key);
      if (label != entry.labels.end() && label->get<std::string>() == label_value) {
        return entry.value;
      }
    }
    return -1.0;
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::store::RunStore> store_;
  cf::store::UnixTime now_ = 0;
};

TEST_F(MetricsTest, CollectsGateIterationDefectAndEscalationMetrics) {
  cf::store::MilestoneRecord milestone;
  milestone.run_id = "R12";
  milestone.milestone_id = "M1";
  milestone.state = "Done";
  milestone.iterations = 2;
  ASSERT_TRUE(store_->upsert_milestone(milestone).has_value());

  cf::store::GateResultRow gate;
  gate.run_id = "R12";
  gate.milestone_id = "M1";
  gate.commit_sha = "abc";
  gate.gate = "build";
  gate.passed = true;
  ASSERT_TRUE(store_->record_gate_result(gate).has_value());
  gate.gate = "guardrails";
  gate.passed = false;
  ASSERT_TRUE(store_->record_gate_result(gate).has_value());

  cf::store::DefectRecord defect;
  defect.id = "D1";
  defect.run_id = "R12";
  defect.milestone_id = "M1";
  defect.status = "OPEN";
  defect.severity = "blocker";
  ASSERT_TRUE(store_->upsert_defect(defect).has_value());

  for (const int64_t latency : {10, 20, 30, 100}) {
    ASSERT_TRUE(store_->record_rag_latency("R12", latency, now_).has_value());
  }

  ASSERT_TRUE(store_
                  ->apply_transition("R12", "Executing", "Escalated",
                                     R"({"event":"budget_exceeded"})", {}, now_)
                  .has_value());

  auto samples = cf::orch::collect_run_metrics(*store_, "R12", dir_);
  ASSERT_TRUE(samples.has_value()) << samples.error().message;
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_gate_pass_ratio", "gate", "build"), 1.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_gate_pass_ratio", "gate", "guardrails"),
                   0.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_iterations", "milestone", "M1"), 2.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_defects_open", "", ""), 1.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_escalations_total", "reason",
                          "budget_exceeded"),
                   1.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_rag_latency_ms", "quantile", "avg"),
                   40.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_rag_latency_ms", "quantile", "p95"),
                   100.0);
}

TEST_F(MetricsTest, CollectsLlmTokensAndUsdFromTheLedgerAndAgentUsage) {
  cf::store::LlmCallRecord call;
  call.run_id = "R12";
  call.role = "coder";
  call.model = "ledger-model";
  call.prompt_tokens = 10;
  call.completion_tokens = 5;
  call.usd = 0.4;
  call.at = now_;
  ASSERT_TRUE(store_->record_llm_call(call).has_value());

  write_file(dir_ / "usage-coder.jsonl",
             R"({"role":"coder","milestone_id":"M1","usage":{"cost":0.2,"model":{"id":"agent-model"},"tokens":{"input":4,"output":2,"cache":{"read":1}}}})"
             "\n");

  auto samples = cf::orch::collect_run_metrics(*store_, "R12", dir_);
  ASSERT_TRUE(samples.has_value()) << samples.error().message;
  auto tokens = [&samples](const std::string& model, const std::string& kind) {
    for (const auto& entry : *samples) {
      if (entry.name != "cf_llm_tokens_total") {
        continue;
      }
      if (entry.labels.at("model") == model && entry.labels.at("kind") == kind) {
        return entry.value;
      }
    }
    return -1.0;
  };
  EXPECT_DOUBLE_EQ(tokens("ledger-model", "prompt"), 10.0);
  EXPECT_DOUBLE_EQ(tokens("ledger-model", "completion"), 5.0);
  EXPECT_DOUBLE_EQ(tokens("agent-model", "prompt"), 4.0);
  EXPECT_DOUBLE_EQ(tokens("agent-model", "completion"), 2.0);
  EXPECT_DOUBLE_EQ(tokens("agent-model", "cached"), 1.0);
  EXPECT_DOUBLE_EQ(sample(*samples, "cf_llm_usd_total", "role", "coder"), 0.6);
}

}  // namespace
