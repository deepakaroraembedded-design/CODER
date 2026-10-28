// Unit tests for waiting-state reminders (design section 12.1).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/cli/commands.hpp"
#include "cf/orch/approval_deadline.hpp"
#include "cf/store/run_store.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_reminder_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class ReminderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    now_ = cf::store::unix_now();
    std::filesystem::create_directories(dir_ / "runs" / "R12");
    auto opened = cf::store::RunStore::open(dir_ / "runs" / "R12" / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Created";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());

    // Find a start instant exactly two business days ago.
    waiting_since_ = now_;
    for (int days = 1; days <= 14; ++days) {
      const auto candidate = now_ - static_cast<int64_t>(days) * 86400;
      if (cf::orch::business_days_between(candidate, now_) == 2) {
        waiting_since_ = candidate;
        break;
      }
    }
    ASSERT_NE(waiting_since_, now_);
    ASSERT_TRUE(store_
                    ->apply_transition("R12", "Created", "AwaitingDesignApproval",
                                       "{}", {}, waiting_since_)
                    .has_value());

    auto lease = store_->acquire_lease("R12", "test", std::chrono::seconds(60),
                                       now_);
    ASSERT_TRUE(lease.has_value());
    cf::store::NewOperation operation;
    operation.idempotency_key = "pr-create-R12";
    operation.kind = "pr_create";
    operation.input_json = "{}";
    operation.expected_json = "{}";
    ASSERT_TRUE(store_->record_intents("R12", {operation}, now_).has_value());
    auto recorded = store_->find_operation_by_key("pr-create-R12");
    ASSERT_TRUE(recorded.has_value());
    ASSERT_TRUE(store_
                    ->mark_operation_started(recorded->op_id, lease->epoch, now_)
                    .has_value());
    ASSERT_TRUE(store_
                    ->confirm_operation(recorded->op_id, R"({"number": 7})",
                                        lease->epoch, now_)
                    .has_value());
    transport_ = std::make_unique<ScriptedHttpTransport>();
  }

  void TearDown() override {
    transport_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  cf::cli::ReminderOptions options() {
    cf::cli::ReminderOptions options;
    options.workspace = dir_;
    options.run_id = "R12";
    options.github_owner = "o";
    options.github_repo = "r";
    options.github_token = "ghs_test";
    options.github_transport = transport_.get();
    return options;
  }

  std::filesystem::path dir_;
  cf::store::UnixTime now_ = 0;
  cf::store::UnixTime waiting_since_ = 0;
  std::unique_ptr<cf::store::RunStore> store_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
};

TEST_F(ReminderTest, PostsTheDueDesignApprovalReminderWithOwners) {
  transport_->script(200, "[]");  // list comments
  transport_->script(200, "* @alice\n");  // CODEOWNERS
  transport_->script(201, R"({"id": 99})");  // create comment

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(cf::cli::reminder_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_EQ(document["elapsed_business_days"], 2);
  ASSERT_EQ(document["posted"].size(), 1u);
  EXPECT_EQ(document["posted"][0], 2);

  bool posted = false;
  for (const auto& request : transport_->requests()) {
    if (request.method == "POST") {
      EXPECT_NE(request.body.find("@alice"), std::string::npos);
      EXPECT_NE(request.body.find("<!-- cf-reminder:design_approval:2 -->"),
                std::string::npos);
      posted = true;
    }
  }
  EXPECT_TRUE(posted);
}

TEST_F(ReminderTest, AllSweepsTheWorkspace) {
  transport_->script(200, "[]");
  transport_->script(200, "* @alice\n");
  transport_->script(201, R"({"id": 99})");

  cf::cli::ReminderOptions all = options();
  all.all = true;
  all.run_id.clear();
  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(cf::cli::reminder_command(all, out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  ASSERT_EQ(document["runs"].size(), 1u);
  ASSERT_EQ(document["runs"][0]["posted"].size(), 1u);
  EXPECT_EQ(document["runs"][0]["posted"][0], 2);
}

TEST_F(ReminderTest, PostsTheDueWaiverRequestReminder) {
  cf::store::WaiverRequestRecord request;
  request.run_id = "R12";
  request.milestone_id = "M1";
  request.rule = "guardrails";
  request.scope = "src/a.cpp:1";
  request.justification = "waiting for a decision";
  request.requested_by = "coder";
  request.requested_at = waiting_since_;
  request.deadline_at = now_ + 1000;
  auto recorded = store_->record_waiver_request(request);
  ASSERT_TRUE(recorded.has_value());

  transport_->script(200, "[]");  // list comments
  transport_->script(200, "* @alice\n");  // CODEOWNERS
  transport_->script(201, R"({"id": 99})");  // design approval reminder
  transport_->script(201, R"({"id": 100})");  // waiver reminder

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(cf::cli::reminder_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  ASSERT_EQ(document["waiver_requests_posted"].size(), 1u);
  EXPECT_EQ(document["waiver_requests_posted"][0], recorded->id);

  bool waiver_comment = false;
  for (const auto& request_sent : transport_->requests()) {
    if (request_sent.method == "POST" &&
        request_sent.body.find("cf-reminder:waiver:" + recorded->id) !=
            std::string::npos) {
      waiver_comment = true;
    }
  }
  EXPECT_TRUE(waiver_comment);
}

TEST_F(ReminderTest, DoesNotRepeatAnExistingReminder) {
  transport_->script(
      200,
      R"([{"id": 1, "body": "already sent\n<!-- cf-reminder:design_approval:2 -->"}])");
  transport_->script(200, "* @alice\n");

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(cf::cli::reminder_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_TRUE(document["posted"].empty());
  ASSERT_EQ(document["skipped"].size(), 1u);
  EXPECT_EQ(document["skipped"][0], 2);
  for (const auto& request : transport_->requests()) {
    EXPECT_NE(request.method, "POST");
  }
}

TEST_F(ReminderTest, DoesNothingWhenTheRunIsNotWaiting) {
  ASSERT_TRUE(store_
                  ->apply_transition("R12", "AwaitingDesignApproval", "Executing",
                                     "{}", {}, now_)
                  .has_value());
  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(cf::cli::reminder_command(options(), out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_EQ(document["reason"], "not_waiting");
  EXPECT_TRUE(transport_->requests().empty());
}

}  // namespace
