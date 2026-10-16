// Unit tests for event-stream routing (design section 6.6.1).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/orch/event_router.hpp"

namespace {

using cf::agents::OpencodeEvent;
using cf::agents::PermissionBroker;
using cf::agents::PermissionDecision;
using cf::agents::PermissionPolicy;
using cf::agents::PermissionPolicyConfig;
using cf::agents::PermissionReply;
using cf::agents::PermissionRequest;
using cf::orch::EventRouterHooks;
using cf::orch::OpencodeEventRouter;
using cf::orch::QuestionAsked;
using cf::orch::QuestionDecision;
using Json = nlohmann::json;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_event_router_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class EventRouterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    worktree_ = make_temp_dir();
    config_.worktree = worktree_.string();
    config_.writable_paths = {"src"};
    config_.allowed_commands = {"cmake", "ctest"};
    config_.deny_commands = {"curl"};
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(worktree_, ec);
  }

  OpencodeEvent permission_event(const std::string& command) const {
    OpencodeEvent event;
    event.type = "permission.asked";
    event.properties = Json{{"id", "per_1"},
                            {"sessionID", "ses_1"},
                            {"permission", "bash"},
                            {"patterns", Json::array({command})},
                            {"metadata", Json{{"command", command}}}};
    return event;
  }

  std::filesystem::path worktree_;
  PermissionPolicyConfig config_;
};

TEST_F(EventRouterTest, PermissionEventsGoToTheBroker) {
  std::vector<PermissionReply> replies;
  PermissionBroker broker(PermissionPolicy(config_),
                          [&replies](const PermissionRequest&, const PermissionDecision& d) {
                            replies.push_back(d.reply);
                          });
  OpencodeEventRouter router("coder", broker, EventRouterHooks{});
  EXPECT_TRUE(router.handle(permission_event("cmake --build /build")));
  EXPECT_TRUE(router.handle(permission_event("rm -rf /")));
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_EQ(replies[0], PermissionReply::Once);
  EXPECT_EQ(replies[1], PermissionReply::Reject);
  EXPECT_EQ(router.permissions_handled(), 2u);
  EXPECT_EQ(broker.rejected_in_turn(), 1u);
}

TEST_F(EventRouterTest, QuestionEventsUseThePolicyHook) {
  PermissionBroker broker(PermissionPolicy(config_), {});
  std::vector<QuestionAsked> asked;
  std::vector<QuestionDecision> decisions;
  EventRouterHooks hooks;
  hooks.reply_question = [&asked, &decisions](const QuestionAsked& question) {
    asked.push_back(question);
    QuestionDecision decision;
    decision.answer = false;
    decision.reason = "no high-precedence document covers it";
    decisions.push_back(decision);
    return decision;
  };
  OpencodeEventRouter router("qa", broker, hooks);

  OpencodeEvent event;
  event.type = "question.asked";
  event.properties = Json{
      {"id", "que_1"},
      {"sessionID", "ses_1"},
      {"questions",
       Json::array({Json{
           {"question", "Should I continue?"},
           {"multiple", false},
           {"options",
            Json::array({Json{{"label", "Yes"}, {"description", "proceed"}},
                         Json{{"label", "No"}, {"description", "stop"}}})}}})}};
  EXPECT_TRUE(router.handle(event));
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].session_id, "ses_1");
  EXPECT_EQ(asked[0].request_id, "que_1");
  ASSERT_EQ(asked[0].questions.size(), 1u);
  EXPECT_EQ(asked[0].questions[0].text, "Should I continue?");
  ASSERT_EQ(asked[0].questions[0].options.size(), 2u);
  EXPECT_EQ(asked[0].questions[0].options[0].label, "Yes");
  EXPECT_EQ(decisions[0].reason, "no high-precedence document covers it");
  EXPECT_EQ(router.questions_handled(), 1u);
}

TEST_F(EventRouterTest, UnrelatedEventsAreIgnored) {
  PermissionBroker broker(PermissionPolicy(config_), {});
  OpencodeEventRouter router("coder", broker, EventRouterHooks{});
  OpencodeEvent event;
  event.type = "session.idle";
  event.properties = Json{{"sessionID", "ses_1"}};
  EXPECT_FALSE(router.handle(event));
  EXPECT_EQ(router.permissions_handled(), 0u);
  EXPECT_EQ(router.questions_handled(), 0u);
}

}  // namespace
