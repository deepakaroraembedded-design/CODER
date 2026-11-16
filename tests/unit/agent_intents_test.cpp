// Unit tests for applying agent MCP intents to the real run store while the
// agent only ever writes a disposable snapshot (design section 6.8.1).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/orch/agent_intents.hpp"
#include "cf/store/run_store.hpp"

namespace {

using Json = nlohmann::json;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_intents_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class AgentIntentsTest : public ::testing::Test {
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
    run.created_at = cf::store::unix_now();
    ASSERT_TRUE(store_->create_run(run).has_value());
  }

  void TearDown() override {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::unique_ptr<cf::store::RunStore> store_;
  std::filesystem::path dir_;
  std::map<std::string, std::string> id_map_;
};

TEST_F(AgentIntentsTest, FileAndVerifyDefectAreAppliedOnce) {
  const std::vector<Json> calls = {
      Json{{"tool", "file_defect"},
           {"role", "qa"},
           {"result_id", "D-1"},
           {"arguments",
            Json{{"defect_json", Json{{"test_id", "t3"},
                                      {"summary", "drops an item"},
                                      {"severity", "major"}}}}}},
      Json{{"tool", "verify_defect"},
           {"role", "qa"},
           {"arguments", Json{{"id", "D-1"}, {"result", "VERIFIED"}}}}};
  const auto applied =
      cf::orch::apply_agent_intents(*store_, "R12", "M3", calls, id_map_);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  auto defect = store_->get_defect("D-1");
  ASSERT_TRUE(defect.has_value()) << defect.error().message;
  EXPECT_EQ(defect->status, "VERIFIED");
  EXPECT_EQ(defect->milestone_id, "M3");
  EXPECT_EQ(defect->severity, "major");

  // Replaying the same intent updates the same fingerprint, never a second
  // defect (the calls file is at-least-once across a crash).
  const auto replayed = cf::orch::apply_agent_intents(
      *store_, "R12", "M3", {calls.front()}, id_map_);
  ASSERT_TRUE(replayed.has_value()) << replayed.error().message;
  auto all = store_->list_defects("R12");
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->size(), 1u);
  EXPECT_EQ(all->front().id, "D-1");
}

TEST_F(AgentIntentsTest, CollidingProvisionalIdGetsAFreshId) {
  cf::store::DefectRecord other;
  other.id = "D-1";
  other.run_id = "R12";
  other.milestone_id = "M1";
  other.status = "OPEN";
  other.severity = "minor";
  other.fingerprint = "fp-other";
  other.body_json = "{}";
  ASSERT_TRUE(store_->upsert_defect(other).has_value());

  const std::vector<Json> calls = {
      Json{{"tool", "file_defect"},
           {"result_id", "D-1"},
           {"arguments",
            Json{{"defect_json", Json{{"test_id", "t9"},
                                      {"summary", "different defect"}}}}}}};
  const auto applied =
      cf::orch::apply_agent_intents(*store_, "R12", "M3", calls, id_map_);
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_EQ(id_map_["D-1"], "D-2");
  auto all = store_->list_defects("R12");
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->size(), 2u);
  EXPECT_TRUE(store_->get_defect("D-1").has_value());
  EXPECT_TRUE(store_->get_defect("D-2").has_value());
}

TEST_F(AgentIntentsTest, SemanticDedupHonoursTheMCPResultId) {
  cf::store::DefectRecord existing;
  existing.id = "D-1";
  existing.run_id = "R12";
  existing.milestone_id = "M1";
  existing.status = "OPEN";
  existing.severity = "major";
  existing.fingerprint = "old-fingerprint";
  existing.body_json = R"({"summary":"ring buffer drops item"})";
  ASSERT_TRUE(store_->upsert_defect(existing).has_value());

  const std::vector<Json> calls = {
      Json{{"tool", "file_defect"},
           {"result_id", "D-1"},
           {"result_deduplicated", true},
           {"arguments",
            Json{{"defect_json", Json{{"test_id", "t2"},
                                      {"summary", "ring buffer loses item"},
                                      {"fingerprint", "new-fingerprint"}}}}}}};
  ASSERT_TRUE(cf::orch::apply_agent_intents(*store_, "R12", "M3", calls,
                                            id_map_)
                  .has_value());
  auto all = store_->list_defects("R12");
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->size(), 1u);
  auto updated = store_->get_defect("D-1");
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->status, "REOPENED");
  EXPECT_EQ(updated->severity, "medium");
}

TEST_F(AgentIntentsTest, TestDisputeCountsAndWaiverIsRecorded) {
  std::vector<Json> first = {
      Json{{"tool", "dispute_test"},
           {"result_id", "D-1"},
           {"arguments",
            Json{{"test_id", "tests/t.cpp"}, {"rationale", "stale assertion"}}}}};
  ASSERT_TRUE(cf::orch::apply_agent_intents(*store_, "R12", "M3", first,
                                            id_map_)
                  .has_value());
  auto dispute = store_->get_defect("D-1");
  ASSERT_TRUE(dispute.has_value());
  EXPECT_EQ(dispute->status, "DISPUTED");
  EXPECT_EQ(Json::parse(dispute->body_json).value("disputes", 0), 1);

  // QA rejected the dispute (the milestone loop marks the record decided).
  auto decided = *dispute;
  Json body = Json::parse(decided.body_json);
  body["decided"] = true;
  decided.body_json = body.dump();
  ASSERT_TRUE(store_->upsert_defect(decided).has_value());

  std::vector<Json> second = {
      Json{{"tool", "dispute_test"},
           {"arguments",
            Json{{"test_id", "tests/t.cpp"}, {"rationale", "still stale"}}}}};
  ASSERT_TRUE(cf::orch::apply_agent_intents(*store_, "R12", "M3", second,
                                            id_map_)
                  .has_value());
  dispute = store_->get_defect("D-1");
  ASSERT_TRUE(dispute.has_value());
  EXPECT_EQ(Json::parse(dispute->body_json).value("disputes", 0), 2);

  const std::vector<Json> waiver = {
      Json{{"tool", "request_waiver"},
           {"role", "qa"},
           {"arguments", Json{{"rule", "guardrails"},
                              {"scope", "src/a.cpp:42"},
                              {"justification", "compatibility window"}}}}};
  ASSERT_TRUE(cf::orch::apply_agent_intents(*store_, "R12", "M3", waiver,
                                            id_map_)
                  .has_value());
  auto requests = store_->list_waiver_requests("R12");
  ASSERT_TRUE(requests.has_value()) << requests.error().message;
  ASSERT_EQ(requests->size(), 1u);
  EXPECT_EQ(requests->front().status, "PENDING");
  EXPECT_EQ(requests->front().requested_by, "qa");
}

}  // namespace
