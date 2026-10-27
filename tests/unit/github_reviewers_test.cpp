// Unit tests for CODEOWNERS parsing and review requests on the ready pull
// request (design sections 10.4.3, 10.4.4).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/github/client.hpp"
#include "cf/github/codeowners.hpp"
#include "cf/orch/github_handlers.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::Client;
using cf::github::ClientOptions;
using cf::orch::DispatchOutcome;
using cf::orch::DispatchResult;
using cf::orch::GithubHandlerContext;
using cf::orch::PrCreateHandler;
using cf::orch::PrUpdateHandler;
using cf::store::OperationRecord;
using cf::test::ScriptedHttpTransport;

Client make_client(ScriptedHttpTransport& transport) {
  ClientOptions options;
  options.api_base = "https://api.github.test";
  options.token = "test-token";
  return Client(options, transport);
}

OperationRecord pr_update_op(bool mark_ready) {
  OperationRecord op;
  op.op_id = "op_pr_update_1";
  op.kind = "pr_update";
  op.input_json = nlohmann::json{{"number", 7},
                                 {"body", "milestones: M3 done"},
                                 {"mark_ready", mark_ready}}
                      .dump();
  return op;
}

const char* kPrJson = R"({"number": 7, "html_url": "https://github.test/pr/7",
                          "state": "open", "draft": false})";

TEST(CodeownersTest, ParsesUsersTeamsAndComments) {
  const auto owners = cf::github::parse_codeowners(R"(
# owners for the whole repo
*            @alice @acme/core
/src/        @bob @alice
docs/        docs@example.com
# trailing comment @ignored
)");
  ASSERT_EQ(owners.users.size(), 2u);
  EXPECT_EQ(owners.users[0], "alice");
  EXPECT_EQ(owners.users[1], "bob");
  ASSERT_EQ(owners.teams.size(), 1u);
  EXPECT_EQ(owners.teams[0], "core");
}

TEST(CodeownersTest, EmptyAndGarbage) {
  EXPECT_TRUE(cf::github::parse_codeowners("").users.empty());
  EXPECT_TRUE(cf::github::parse_codeowners("# only a comment\n").users.empty());
  EXPECT_TRUE(cf::github::parse_codeowners("*\n").users.empty());
  EXPECT_TRUE(cf::github::parse_codeowners("@\n* @\n").users.empty());
}

TEST(GithubReviewersTest, RequestsReviewers) {
  ScriptedHttpTransport transport;
  transport.script(201, R"({"number": 7})");
  Client client = make_client(transport);
  const auto result = client.request_reviewers("o", "r", 7, {"alice"}, {"core"});
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(transport.requests().size(), 1u);
  const auto& request = transport.requests()[0];
  EXPECT_EQ(request.method, "POST");
  EXPECT_NE(request.url.find("/repos/o/r/pulls/7/requested_reviewers"),
            std::string::npos);
  const auto payload = nlohmann::json::parse(request.body);
  EXPECT_EQ(payload.at("reviewers").at(0).get<std::string>(), "alice");
  EXPECT_EQ(payload.at("team_reviewers").at(0).get<std::string>(), "core");
}

TEST(GithubReviewersTest, GetFileRawAndErrors) {
  ScriptedHttpTransport transport;
  transport.script(200, "@alice\n");
  Client client = make_client(transport);
  auto content = client.get_file("o", "r", ".github/CODEOWNERS", "cf/R1/integration");
  ASSERT_TRUE(content.has_value()) << content.error().message;
  EXPECT_EQ(*content, "@alice\n");
  ASSERT_EQ(transport.requests().size(), 1u);
  EXPECT_NE(transport.requests()[0].url.find("?ref=cf%2FR1%2Fintegration"),
            std::string::npos);
  bool raw_accept = false;
  for (const auto& [key, value] : transport.requests()[0].headers) {
    raw_accept = raw_accept || (key == "Accept" && value == "application/vnd.github.raw");
  }
  EXPECT_TRUE(raw_accept);

  ScriptedHttpTransport missing;
  missing.script(404, R"({"message": "Not Found"})");
  Client missing_client = make_client(missing);
  auto not_found = missing_client.get_file("o", "r", "CODEOWNERS");
  ASSERT_FALSE(not_found.has_value());
  EXPECT_EQ(not_found.error().code, cf::github::error_codes::kNotFound);

  ScriptedHttpTransport down;
  down.script(503, R"({"message": "unavailable"})");
  Client down_client = make_client(down);
  auto unavailable = down_client.get_file("o", "r", "CODEOWNERS");
  ASSERT_FALSE(unavailable.has_value());
  EXPECT_EQ(unavailable.error().code, cf::github::error_codes::kServerError);
}

TEST(PrReviewersTest, MarkReadyRequestsCodeownersReviewers) {
  ScriptedHttpTransport transport;
  transport.script(200, kPrJson);
  transport.script(404, R"({"message": "Not Found"})");  // .github/CODEOWNERS
  transport.script(200, "* @alice @acme/core\n");        // CODEOWNERS
  transport.script(201, R"({"number": 7})");             // request reviewers
  Client client = make_client(transport);
  GithubHandlerContext context;
  context.client = &client;
  context.owner = "acme";
  context.repo = "product-y";
  PrUpdateHandler handler(context);

  const DispatchResult result = handler.perform(pr_update_op(true));
  ASSERT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  ASSERT_EQ(transport.requests().size(), 4u);
  EXPECT_EQ(transport.requests()[1].method, "GET");
  EXPECT_NE(transport.requests()[1].url.find("/contents/.github/CODEOWNERS"),
            std::string::npos);
  EXPECT_NE(transport.requests()[2].url.find("/contents/CODEOWNERS"),
            std::string::npos);
  const auto payload = nlohmann::json::parse(transport.requests()[3].body);
  EXPECT_EQ(payload.at("reviewers").at(0).get<std::string>(), "alice");
  EXPECT_EQ(payload.at("team_reviewers").at(0).get<std::string>(), "core");
}

TEST(PrReviewersTest, MissingCodeownersFileIsFine) {
  ScriptedHttpTransport transport;
  transport.script(200, kPrJson);
  transport.script(404, R"({"message": "Not Found"})");
  transport.script(404, R"({"message": "Not Found"})");
  transport.script(404, R"({"message": "Not Found"})");
  Client client = make_client(transport);
  GithubHandlerContext context;
  context.client = &client;
  context.owner = "acme";
  context.repo = "product-y";
  PrUpdateHandler handler(context);

  const DispatchResult result = handler.perform(pr_update_op(true));
  EXPECT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  EXPECT_EQ(transport.requests().size(), 4u);  // no reviewer request
}

TEST(PrReviewersTest, RequestCodeownersCanBeDisabled) {
  ScriptedHttpTransport transport;
  transport.script(200, kPrJson);
  Client client = make_client(transport);
  GithubHandlerContext context;
  context.client = &client;
  context.owner = "acme";
  context.repo = "product-y";
  PrUpdateHandler handler(context);

  OperationRecord op = pr_update_op(true);
  op.input_json = nlohmann::json{{"number", 7},
                                 {"body", "milestones: M3 done"},
                                 {"mark_ready", true},
                                 {"request_codeowners", false}}
                      .dump();
  const DispatchResult result = handler.perform(op);
  ASSERT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  EXPECT_EQ(transport.requests().size(), 1u);  // update only
}

TEST(PrReviewersTest, ReadyAtCreationRequestsCodeowners) {
  ScriptedHttpTransport transport;
  transport.script(200, "[]");  // no existing PR
  transport.script(201, kPrJson);
  transport.script(200, "* @alice\n");
  transport.script(201, R"({"number": 7})");
  Client client = make_client(transport);
  GithubHandlerContext context;
  context.client = &client;
  context.owner = "acme";
  context.repo = "product-y";
  PrCreateHandler handler(context);

  OperationRecord op;
  op.op_id = "op_pr_ready";
  op.kind = "pr_create";
  op.input_json =
      nlohmann::json{{"base", "main"},
                     {"head", "cf/R1/integration"},
                     {"title", "CF R1"},
                     {"body", "summary"},
                     {"draft", false},
                     {"request_codeowners", true}}
          .dump();
  const DispatchResult result = handler.perform(op);
  ASSERT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  ASSERT_EQ(transport.requests().size(), 4u);
  const auto payload = nlohmann::json::parse(transport.requests()[3].body);
  EXPECT_EQ(payload.at("reviewers").at(0).get<std::string>(), "alice");
}

TEST(PrReviewersTest, DraftUpdateDoesNotRequestReviewers) {
  ScriptedHttpTransport transport;
  transport.script(200, kPrJson);
  Client client = make_client(transport);
  GithubHandlerContext context;
  context.client = &client;
  context.owner = "acme";
  context.repo = "product-y";
  PrUpdateHandler handler(context);

  const DispatchResult result = handler.perform(pr_update_op(false));
  EXPECT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  EXPECT_EQ(transport.requests().size(), 1u);
}

TEST(PrReviewersTest, ReconcileRequestsReviewersAfterACrash) {
  const OperationRecord op = pr_update_op(true);
  const std::string body =
      "milestones: M3 done\n\n" + Client::op_marker(op.op_id);
  ScriptedHttpTransport transport;
  transport.script(200, nlohmann::json{{"number", 7},
                                       {"html_url", "https://github.test/pr/7"},
                                       {"state", "open"},
                                       {"draft", false},
                                       {"body", body}}
                            .dump());
  transport.script(200, "* @carol\n");
  transport.script(201, R"({"number": 7})");
  Client client = make_client(transport);
  GithubHandlerContext context;
  context.client = &client;
  context.owner = "acme";
  context.repo = "product-y";
  PrUpdateHandler handler(context);

  const DispatchResult result = handler.reconcile(op);
  ASSERT_EQ(result.outcome, DispatchOutcome::Confirmed) << result.detail;
  ASSERT_EQ(transport.requests().size(), 3u);
  const auto payload = nlohmann::json::parse(transport.requests()[2].body);
  EXPECT_EQ(payload.at("reviewers").at(0).get<std::string>(), "carol");
}

}  // namespace
