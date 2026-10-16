// Unit tests for the GitHub REST client (design sections 6.10, 10.4).
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cf/github/client.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::Client;
using cf::github::ClientOptions;
using cf::test::ScriptedHttpTransport;

class GithubClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_unique<ScriptedHttpTransport>();
    ClientOptions options;
    options.api_base = "https://api.github.test";
    options.token = "ghs_test";
    client_ = std::make_unique<Client>(options, *transport_);
  }

  std::unique_ptr<ScriptedHttpTransport> transport_;
  std::unique_ptr<Client> client_;
};

TEST_F(GithubClientTest, CreatesAPullRequest) {
  transport_->script(201, R"({
    "number": 7, "html_url": "https://github.test/o/r/pull/7", "state": "open",
    "body": "summary", "draft": true,
    "head": {"ref": "cf/R12/integration"}, "base": {"ref": "main"}})");
  auto pr = client_->create_pull_request("o", "r", "CF R12", "cf/R12/integration",
                                         "main", "summary", true);
  ASSERT_TRUE(pr.has_value()) << pr.error().message;
  EXPECT_EQ(pr->number, 7);
  EXPECT_EQ(pr->head_branch, "cf/R12/integration");
  EXPECT_TRUE(pr->draft);

  const auto& request = transport_->requests().front();
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.url, "https://api.github.test/repos/o/r/pulls");
  EXPECT_NE(request.body.find("\"head\":\"cf/R12/integration\""), std::string::npos);
  bool has_auth = false;
  for (const auto& [key, value] : request.headers) {
    if (key == "Authorization") {
      has_auth = value == "Bearer ghs_test";
    }
  }
  EXPECT_TRUE(has_auth);
}

TEST_F(GithubClientTest, ListsPullRequestsByHead) {
  transport_->script(200, R"([{"number": 7, "html_url": "u", "state": "open",
                               "body": "b", "draft": false,
                               "head": {"ref": "cf/R12/integration"},
                               "base": {"ref": "main"}}])");
  auto prs = client_->list_open_pull_requests("o", "r", "cf/R12/integration");
  ASSERT_TRUE(prs.has_value()) << prs.error().message;
  ASSERT_EQ(prs->size(), 1u);
  const auto& request = transport_->requests().front();
  EXPECT_EQ(request.method, "GET");
  EXPECT_NE(request.url.find("state=open"), std::string::npos);
  EXPECT_NE(request.url.find("head=o%3Acf%2FR12%2Fintegration"), std::string::npos);
}

TEST_F(GithubClientTest, MapsClientErrors) {
  transport_->script(422, R"({"message": "Validation Failed"})");
  auto pr = client_->create_pull_request("o", "r", "t", "h", "main", "", true);
  ASSERT_FALSE(pr.has_value());
  EXPECT_EQ(pr.error().code, cf::github::error_codes::kClientError);
  EXPECT_NE(pr.error().message.find("Validation Failed"), std::string::npos);
}

TEST_F(GithubClientTest, MapsServerErrors) {
  transport_->script(500, R"({"message": "boom"})");
  auto pr = client_->create_pull_request("o", "r", "t", "h", "main", "", true);
  ASSERT_FALSE(pr.has_value());
  EXPECT_EQ(pr.error().code, cf::github::error_codes::kServerError);
}

TEST_F(GithubClientTest, MapsTransportErrors) {
  transport_->script_transport_error("connect failed");
  auto pr = client_->create_pull_request("o", "r", "t", "h", "main", "", true);
  ASSERT_FALSE(pr.has_value());
  EXPECT_EQ(pr.error().code, cf::github::error_codes::kTransportError);
}

TEST_F(GithubClientTest, CreatesIssuesAndComments) {
  transport_->script(201, R"({"number": 3, "html_url": "u", "title": "t",
                               "body": "b", "state": "open"})");
  transport_->script(201, R"({"id": 99})");
  auto issue = client_->create_issue("o", "r", "Duplicate ids", "details", {"cf-defect"});
  ASSERT_TRUE(issue.has_value()) << issue.error().message;
  EXPECT_EQ(issue->number, 3);
  EXPECT_TRUE(client_->create_comment("o", "r", 3, "fixed").has_value());
  EXPECT_EQ(transport_->requests()[1].method, "POST");
  EXPECT_NE(transport_->requests()[1].url.find("/issues/3/comments"), std::string::npos);
}

TEST_F(GithubClientTest, ListsIssuesSkipsPullRequests) {
  transport_->script(200, R"([
    {"number": 1, "html_url": "u1", "title": "defect", "body": "b", "state": "open"},
    {"number": 2, "html_url": "u2", "title": "a PR", "body": "b", "state": "open",
     "pull_request": {"url": "x"}}])");
  auto issues = client_->list_issues("o", "r");
  ASSERT_TRUE(issues.has_value()) << issues.error().message;
  ASSERT_EQ(issues->size(), 1u);
  EXPECT_EQ(issues->front().number, 1);
  EXPECT_NE(transport_->requests().front().url.find("state=open"), std::string::npos);

  transport_->script(200, "[]");
  ASSERT_TRUE(client_->list_issues("o", "r", "all").has_value());
  EXPECT_NE(transport_->requests()[1].url.find("state=all"), std::string::npos);
}

TEST_F(GithubClientTest, GetsAnIssue) {
  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "b", "state": "closed"})");
  auto issue = client_->get_issue("o", "r", 5);
  ASSERT_TRUE(issue.has_value()) << issue.error().message;
  EXPECT_EQ(issue->state, "closed");
  EXPECT_EQ(transport_->requests().front().url,
            "https://api.github.test/repos/o/r/issues/5");
}

TEST_F(GithubClientTest, UpdatesUsePatch) {
  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "new", "state": "closed"})");
  auto issue = client_->update_issue("o", "r", 5, "new", "closed");
  ASSERT_TRUE(issue.has_value()) << issue.error().message;
  EXPECT_EQ(transport_->requests().front().method, "PATCH");
  EXPECT_NE(transport_->requests().front().body.find("\"state\":\"closed\""),
            std::string::npos);
}

TEST_F(GithubClientTest, GetsAPullRequest) {
  transport_->script(200, R"({"number": 7, "html_url": "u", "state": "open",
                               "body": "body", "draft": true,
                               "head": {"ref": "cf/R12/integration"},
                               "base": {"ref": "main"}})");
  auto pr = client_->get_pull_request("o", "r", 7);
  ASSERT_TRUE(pr.has_value()) << pr.error().message;
  EXPECT_EQ(pr->number, 7);
  EXPECT_EQ(transport_->requests().front().url,
            "https://api.github.test/repos/o/r/pulls/7");
}

TEST_F(GithubClientTest, ListsIssueComments) {
  transport_->script(200, R"([{"body": "first"}, {"body": "second"}])");
  auto comments = client_->list_issue_comments("o", "r", 3);
  ASSERT_TRUE(comments.has_value()) << comments.error().message;
  ASSERT_EQ(comments->size(), 2u);
  EXPECT_EQ((*comments)[1], "second");
  EXPECT_NE(transport_->requests().front().url.find("/issues/3/comments"),
            std::string::npos);
}

TEST(GithubMarkerTest, MarkersRoundTrip) {
  const std::string marker = Client::op_marker("op_7f3a");
  EXPECT_EQ(marker, "<!-- cf-op: op_7f3a -->");
  EXPECT_TRUE(Client::body_has_marker("body\n\n" + marker, "op_7f3a"));
  EXPECT_FALSE(Client::body_has_marker("body", "op_7f3a"));

  const std::string fingerprint = Client::fingerprint_marker("sha256:abc");
  EXPECT_EQ(fingerprint, "<!-- cf-fingerprint: sha256:abc -->");
  EXPECT_TRUE(Client::body_has_fingerprint("body\n" + fingerprint, "sha256:abc"));
  EXPECT_FALSE(Client::body_has_fingerprint("body", "sha256:abc"));
}

}  // namespace
