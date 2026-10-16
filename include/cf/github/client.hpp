// cf-github: the GitHub REST client (design sections 6.10, 10.4).
//
// Minimal surface for the orchestrator's operations: pull requests, issues
// and comments, with the hidden `cf-op` markers that make recovery
// idempotent (design sections 6.12.1, 6.12.6). All calls go through the
// shared HTTP transport, so tests inject recorded fixtures.
#pragma once

#include <chrono>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/llm/client.hpp"

namespace cf::github {

namespace error_codes {
inline constexpr const char* kTransportError = "GITHUB_TRANSPORT_ERROR";
inline constexpr const char* kClientError = "GITHUB_CLIENT_ERROR";  // 4xx
inline constexpr const char* kServerError = "GITHUB_SERVER_ERROR";  // 5xx
inline constexpr const char* kInvalidResponse = "GITHUB_INVALID_RESPONSE";
inline constexpr const char* kNotFound = "GITHUB_NOT_FOUND";  // 404 on a file
}  // namespace error_codes

struct ClientOptions {
  std::string api_base = "https://api.github.com";
  std::string token;  // short-lived installation token (design section 10.4.2)
  // When set, overrides `token`: called per request so an expiring
  // installation token is refreshed transparently (AppAuth).
  std::function<Result<std::string>()> token_provider;
  std::string user_agent = "swiftrade-cf-bot";
  std::chrono::milliseconds timeout{30000};
};

struct PullRequest {
  int64_t number = 0;
  std::string html_url;
  std::string state;
  std::string head_branch;
  std::string base_branch;
  std::string body;
  bool draft = false;
};

struct Issue {
  int64_t number = 0;
  std::string html_url;
  std::string title;
  std::string body;
  std::string state;
};

class Client {
 public:
  Client(ClientOptions options, llm::HttpTransport& transport);

  Result<PullRequest> create_pull_request(const std::string& owner,
                                          const std::string& repo,
                                          const std::string& title,
                                          const std::string& head,
                                          const std::string& base,
                                          const std::string& body, bool draft);

  // Open pull requests whose head branch is `head` (any owner).
  Result<std::vector<PullRequest>> list_open_pull_requests(const std::string& owner,
                                                           const std::string& repo,
                                                           const std::string& head);

  Result<PullRequest> update_pull_request(const std::string& owner,
                                          const std::string& repo, int64_t number,
                                          const std::string& body,
                                          bool mark_ready = false,
                                          const std::string& state = "");

  Result<PullRequest> get_pull_request(const std::string& owner,
                                       const std::string& repo, int64_t number);

  // Requests reviewers on a pull request (design section 10.4.4): users are
  // GitHub logins, teams are slugs (the part after the org in CODEOWNERS).
  Status request_reviewers(const std::string& owner, const std::string& repo,
                           int64_t number, const std::vector<std::string>& users,
                           const std::vector<std::string>& teams);

  // Raw file content at `ref` (default: the repository's default branch).
  // A missing file is GITHUB_NOT_FOUND.
  Result<std::string> get_file(const std::string& owner, const std::string& repo,
                               const std::string& path, const std::string& ref = "");

  Result<std::vector<std::string>> list_issue_comments(const std::string& owner,
                                                       const std::string& repo,
                                                       int64_t issue_number);

  Result<Issue> create_issue(const std::string& owner, const std::string& repo,
                             const std::string& title, const std::string& body,
                             const std::vector<std::string>& labels);

  // state: "open", "closed" or "all"; pull requests are filtered out.
  Result<std::vector<Issue>> list_issues(const std::string& owner,
                                         const std::string& repo,
                                         const std::string& state = "open");

  Result<Issue> get_issue(const std::string& owner, const std::string& repo,
                          int64_t number);

  Result<Issue> update_issue(const std::string& owner, const std::string& repo,
                             int64_t number, const std::string& body,
                             const std::string& state);

  Status create_comment(const std::string& owner, const std::string& repo,
                        int64_t issue_number, const std::string& body);

  // Hidden markers: `<!-- cf-op: op_... -->` and
  // `<!-- cf-fingerprint: ... -->` (design section 6.10).
  static std::string op_marker(std::string_view op_id);
  static bool body_has_marker(const std::string& body, std::string_view op_id);
  static std::string fingerprint_marker(std::string_view fingerprint);
  static bool body_has_fingerprint(const std::string& body,
                                   std::string_view fingerprint);

 private:
  Result<llm::Json> request(const std::string& method, const std::string& path,
                            const llm::Json& body);
  Result<llm::HttpResponse> send(const std::string& method, const std::string& path,
                                 const llm::Json& body, const std::string& accept);

  ClientOptions options_;
  llm::HttpTransport& transport_;
};

}  // namespace cf::github
