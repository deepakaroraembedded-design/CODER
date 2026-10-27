#include "cf/github/client.hpp"

#include <cctype>
#include <utility>

namespace cf::github {
namespace {

using llm::Json;

std::string url_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[uc >> 4]);
      out.push_back(kHex[uc & 0x0f]);
    }
  }
  return out;
}

Result<PullRequest> parse_pull_request(const Json& json) {
  if (!json.is_object() || !json.contains("number")) {
    return fail(error_codes::kInvalidResponse, "pull request payload is missing 'number'");
  }
  PullRequest pr;
  pr.number = json.value("number", int64_t{0});
  pr.html_url = json.value("html_url", "");
  pr.state = json.value("state", "");
  pr.body = json.value("body", "");
  pr.draft = json.value("draft", false);
  const auto head = json.find("head");
  if (head != json.end() && head->is_object()) {
    pr.head_branch = head->value("ref", "");
  }
  const auto base = json.find("base");
  if (base != json.end() && base->is_object()) {
    pr.base_branch = base->value("ref", "");
  }
  return pr;
}

Result<Issue> parse_issue(const Json& json) {
  if (!json.is_object() || !json.contains("number")) {
    return fail(error_codes::kInvalidResponse, "issue payload is missing 'number'");
  }
  Issue issue;
  issue.number = json.value("number", int64_t{0});
  issue.html_url = json.value("html_url", "");
  issue.title = json.value("title", "");
  issue.body = json.value("body", "");
  issue.state = json.value("state", "");
  return issue;
}

std::string error_detail(const llm::HttpResponse& response) {
  const Json parsed = Json::parse(response.body, nullptr, false);
  if (!parsed.is_discarded() && parsed.is_object()) {
    return parsed.value("message", response.body);
  }
  return response.body;
}

}  // namespace

Client::Client(ClientOptions options, llm::HttpTransport& transport)
    : options_(std::move(options)), transport_(transport) {}

std::string Client::op_marker(std::string_view op_id) {
  return "<!-- cf-op: " + std::string(op_id) + " -->";
}

bool Client::body_has_marker(const std::string& body, std::string_view op_id) {
  return body.find(op_marker(op_id)) != std::string::npos;
}

std::string Client::fingerprint_marker(std::string_view fingerprint) {
  return "<!-- cf-fingerprint: " + std::string(fingerprint) + " -->";
}

bool Client::body_has_fingerprint(const std::string& body,
                                  std::string_view fingerprint) {
  return body.find(fingerprint_marker(fingerprint)) != std::string::npos;
}

Result<llm::HttpResponse> Client::send(const std::string& method,
                                     const std::string& path, const Json& body,
                                     const std::string& accept) {
  std::string token = options_.token;
  if (options_.token_provider) {
    auto fresh = options_.token_provider();
    if (!fresh) {
      return tl::make_unexpected(fresh.error());
    }
    token = *fresh;
  }
  llm::HttpRequest request;
  request.url = options_.api_base + path;
  request.headers = {{"Authorization", "Bearer " + token},
                     {"Accept", accept},
                     {"X-GitHub-Api-Version", "2022-11-28"},
                     {"User-Agent", options_.user_agent},
                     {"Content-Type", "application/json"}};
  request.body = body.is_null() ? std::string() : body.dump();
  request.timeout = options_.timeout;

  Result<llm::HttpResponse> response =
      method == "GET"      ? transport_.get(request)
      : method == "POST"   ? transport_.post(request)
      : method == "DELETE" ? transport_.del(request)
                           : transport_.patch(request);
  if (!response) {
    return fail(error_codes::kTransportError, response.error().message);
  }
  return *response;
}

Result<Json> Client::request(const std::string& method, const std::string& path,
                             const Json& body) {
  auto response = send(method, path, body, "application/vnd.github+json");
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  const long status = response->status;
  if (status >= 200 && status < 300) {
    const Json parsed = Json::parse(response->body, nullptr, false);
    if (parsed.is_discarded()) {
      return fail(error_codes::kInvalidResponse,
                  "GitHub returned a non-JSON body (HTTP " + std::to_string(status) + ")");
    }
    return parsed;
  }
  if (status >= 500) {
    return fail(error_codes::kServerError,
                "GitHub HTTP " + std::to_string(status) + ": " + error_detail(*response));
  }
  return fail(error_codes::kClientError,
              "GitHub HTTP " + std::to_string(status) + ": " + error_detail(*response));
}

Result<PullRequest> Client::create_pull_request(
    const std::string& owner, const std::string& repo, const std::string& title,
    const std::string& head, const std::string& base, const std::string& body,
    bool draft) {
  const Json payload = {{"title", title},   {"head", head}, {"base", base},
                        {"body", body},     {"draft", draft}};
  auto response = request("POST", "/repos/" + owner + "/" + repo + "/pulls", payload);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return parse_pull_request(*response);
}

Result<std::vector<PullRequest>> Client::list_open_pull_requests(
    const std::string& owner, const std::string& repo, const std::string& head) {
  auto response = request("GET",
                          "/repos/" + owner + "/" + repo + "/pulls?state=open&head=" +
                              url_encode(owner + ":" + head),
                          Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (!response->is_array()) {
    return fail(error_codes::kInvalidResponse, "pull request list is not an array");
  }
  std::vector<PullRequest> out;
  for (const auto& entry : *response) {
    auto pr = parse_pull_request(entry);
    if (!pr) {
      return tl::make_unexpected(pr.error());
    }
    out.push_back(std::move(*pr));
  }
  return out;
}

Result<PullRequest> Client::update_pull_request(const std::string& owner,
                                                const std::string& repo,
                                                int64_t number,
                                                const std::string& body,
                                                bool mark_ready,
                                                const std::string& state) {
  Json payload = {{"body", body}};
  if (mark_ready) {
    payload["draft"] = false;
  }
  if (!state.empty()) {
    payload["state"] = state;
  }
  auto response = request(
      "PATCH", "/repos/" + owner + "/" + repo + "/pulls/" + std::to_string(number),
      payload);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return parse_pull_request(*response);
}

Status Client::add_labels(const std::string& owner, const std::string& repo,
                          int64_t number,
                          const std::vector<std::string>& labels) {
  if (labels.empty()) {
    return {};
  }
  auto response = request(
      "POST",
      "/repos/" + owner + "/" + repo + "/issues/" + std::to_string(number) +
          "/labels",
      Json{{"labels", labels}});
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Result<Issue> Client::create_issue(const std::string& owner, const std::string& repo,
                                   const std::string& title, const std::string& body,
                                   const std::vector<std::string>& labels) {
  const Json payload = {{"title", title}, {"body", body}, {"labels", labels}};
  auto response = request("POST", "/repos/" + owner + "/" + repo + "/issues", payload);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return parse_issue(*response);
}

Result<std::vector<Issue>> Client::list_issues(const std::string& owner,
                                               const std::string& repo,
                                               const std::string& state) {
  auto response = request("GET",
                          "/repos/" + owner + "/" + repo + "/issues?state=" +
                              url_encode(state) + "&per_page=100",
                          Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (!response->is_array()) {
    return fail(error_codes::kInvalidResponse, "issue list is not an array");
  }
  std::vector<Issue> out;
  for (const auto& entry : *response) {
    if (!entry.is_object() || entry.contains("pull_request")) {
      continue;  // the issues endpoint also returns pull requests
    }
    auto issue = parse_issue(entry);
    if (!issue) {
      return tl::make_unexpected(issue.error());
    }
    out.push_back(std::move(*issue));
  }
  return out;
}

Result<Issue> Client::get_issue(const std::string& owner, const std::string& repo,
                                int64_t number) {
  auto response = request(
      "GET", "/repos/" + owner + "/" + repo + "/issues/" + std::to_string(number),
      Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return parse_issue(*response);
}

Result<Issue> Client::update_issue(const std::string& owner, const std::string& repo,
                                   int64_t number, const std::string& body,
                                   const std::string& state) {
  Json payload = Json::object();
  if (!body.empty()) {
    payload["body"] = body;
  }
  if (!state.empty()) {
    payload["state"] = state;
  }
  auto response = request(
      "PATCH", "/repos/" + owner + "/" + repo + "/issues/" + std::to_string(number),
      payload);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return parse_issue(*response);
}

Result<PullRequest> Client::get_pull_request(const std::string& owner,
                                            const std::string& repo, int64_t number) {
  auto response = request(
      "GET", "/repos/" + owner + "/" + repo + "/pulls/" + std::to_string(number),
      Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return parse_pull_request(*response);
}

Status Client::request_reviewers(const std::string& owner, const std::string& repo,
                                 int64_t number, const std::vector<std::string>& users,
                                 const std::vector<std::string>& teams) {
  const Json payload = {{"reviewers", users}, {"team_reviewers", teams}};
  auto response = request("POST",
                          "/repos/" + owner + "/" + repo + "/pulls/" +
                              std::to_string(number) + "/requested_reviewers",
                          payload);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Result<std::string> Client::get_file(const std::string& owner, const std::string& repo,
                                     const std::string& path, const std::string& ref) {
  std::string url = "/repos/" + owner + "/" + repo + "/contents/" + path;
  if (!ref.empty()) {
    url += "?ref=" + url_encode(ref);
  }
  auto response = send("GET", url, Json(), "application/vnd.github.raw");
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  const long status = response->status;
  if (status == 404) {
    return fail(error_codes::kNotFound, path + " not found in " + owner + "/" + repo);
  }
  if (status >= 200 && status < 300) {
    return response->body;
  }
  if (status >= 500) {
    return fail(error_codes::kServerError,
                "GitHub HTTP " + std::to_string(status) + ": " + error_detail(*response));
  }
  return fail(error_codes::kClientError,
              "GitHub HTTP " + std::to_string(status) + ": " + error_detail(*response));
}

Result<std::vector<Client::IssueComment>> Client::list_issue_comments(
    const std::string& owner, const std::string& repo, int64_t issue_number) {
  auto response = request(
      "GET",
      "/repos/" + owner + "/" + repo + "/issues/" + std::to_string(issue_number) +
          "/comments?per_page=100",
      Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (!response->is_array()) {
    return fail(error_codes::kInvalidResponse, "comment list is not an array");
  }
  std::vector<IssueComment> comments;
  for (const auto& entry : *response) {
    if (entry.is_object()) {
      comments.push_back(IssueComment{entry.value("id", int64_t{0}),
                                      entry.value("body", "")});
    }
  }
  return comments;
}

Status Client::delete_comment(const std::string& owner, const std::string& repo,
                              int64_t comment_id) {
  // 204 No Content: bypass request()'s JSON parsing.
  auto response = send("DELETE",
                       "/repos/" + owner + "/" + repo + "/issues/comments/" +
                           std::to_string(comment_id),
                       Json(), "application/vnd.github+json");
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (response->status >= 200 && response->status < 300) {
    return {};
  }
  if (response->status >= 500) {
    return fail(error_codes::kServerError,
                "GitHub HTTP " + std::to_string(response->status) + ": " +
                    error_detail(*response));
  }
  return fail(error_codes::kClientError,
              "GitHub HTTP " + std::to_string(response->status) + ": " +
                  error_detail(*response));
}

Status Client::create_comment(const std::string& owner, const std::string& repo,
                              int64_t issue_number, const std::string& body) {
  auto response = request(
      "POST",
      "/repos/" + owner + "/" + repo + "/issues/" + std::to_string(issue_number) +
          "/comments",
      Json{{"body", body}});
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

}  // namespace cf::github
