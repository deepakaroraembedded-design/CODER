#include "cf/orch/github_handlers.hpp"

#include "cf/github/codeowners.hpp"

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace cf::orch {
namespace {

using Json = nlohmann::json;

DispatchResult confirmed(std::string result_json) {
  return DispatchResult{DispatchOutcome::Confirmed, std::move(result_json), ""};
}

DispatchResult retry(std::string detail) {
  return DispatchResult{DispatchOutcome::Retry, "", std::move(detail)};
}

DispatchResult failed(std::string detail) {
  return DispatchResult{DispatchOutcome::Failed, "", std::move(detail)};
}

DispatchResult reconcile_needed(std::string detail) {
  return DispatchResult{DispatchOutcome::NeedsReconcile, "", std::move(detail)};
}

// Transport and server failures are retryable; a 4xx is a policy answer.
DispatchResult classify(const cf::Error& error) {
  if (error.code == github::error_codes::kClientError) {
    return failed(error.message);
  }
  return retry(error.message);
}

// Design section 10.4.4: marking the pull request ready requests the
// CODEOWNERS reviewers. GitHub's lookup order is .github/, root, docs/;
// a missing file simply means no reviewers to request. Re-requesting is
// idempotent on GitHub's side, so reconciliation may repeat it.
Result<bool> request_codeowners_reviewers(const GithubHandlerContext& context,
                                          int64_t number) {
  for (const char* path : {".github/CODEOWNERS", "CODEOWNERS", "docs/CODEOWNERS"}) {
    auto file = context.client->get_file(context.owner, context.repo, path);
    if (!file) {
      if (file.error().code == github::error_codes::kNotFound) {
        continue;
      }
      return tl::make_unexpected(file.error());
    }
    const github::Codeowners owners = github::parse_codeowners(*file);
    if (owners.users.empty() && owners.teams.empty()) {
      return true;
    }
    auto requested = context.client->request_reviewers(context.owner, context.repo,
                                                       number, owners.users, owners.teams);
    if (!requested) {
      return tl::make_unexpected(requested.error());
    }
    return true;
  }
  return true;
}

Json parse_or_object(const std::string& text) {
  Json parsed = Json::parse(text, nullptr, false);
  return parsed.is_discarded() ? Json::object() : parsed;
}

Status apply_labels(const GithubHandlerContext& context, int64_t number,
                    const Json& input) {
  const auto labels = input.find("labels");
  if (labels == input.end() || !labels->is_array() || labels->empty()) {
    return {};
  }
  std::vector<std::string> names;
  for (const auto& label : *labels) {
    names.push_back(label.get<std::string>());
  }
  return context.client->add_labels(context.owner, context.repo, number, names);
}

}  // namespace

PushHandler::PushHandler(GithubHandlerContext context) : context_(std::move(context)) {}

DispatchResult PushHandler::sync(const store::OperationRecord& op, bool performing) {
  const Json input = parse_or_object(op.input_json);
  const Json expected = parse_or_object(op.expected_json);
  const std::string remote = input.value("remote", context_.remote);
  const std::string branch = input.value("branch", "");
  const std::string commit = input.value("commit", "");
  const std::string old = expected.value("old", "");
  if (branch.empty() || commit.empty()) {
    return failed("push intent needs a branch and a commit");
  }
  if (context_.repository == nullptr) {
    return failed("no repository configured for push");
  }

  auto current = context_.repository->remote_branch_sha(remote, branch);
  if (!current) {
    return retry("cannot read the remote branch: " + current.error().message);
  }
  if (*current == commit) {
    return confirmed(Json{{"commit", commit}, {"branch", branch}}.dump());
  }
  if (!current->empty()) {
    if (old.empty()) {
      return reconcile_needed("remote branch " + branch + " exists at " + *current +
                             " but the intent expected it to be absent");
    }
    if (*current != old) {
      return reconcile_needed("remote branch " + branch + " is at " + *current +
                             ", expected " + old + "; never force-push over this");
    }
  }
  if (!performing) {
    return retry("the push has not landed yet");
  }

  std::vector<std::string> args = {"push"};
  if (!old.empty()) {
    args.push_back("--force-with-lease=refs/heads/" + branch + ":" + old);
  }
  args.push_back(remote);
  args.push_back(commit + ":refs/heads/" + branch);
  auto pushed = context_.repository->command(args);
  if (!pushed) {
    return retry("push failed: " + pushed.error().message);
  }
  return confirmed(Json{{"commit", commit}, {"branch", branch}}.dump());
}

DispatchResult PushHandler::perform(const store::OperationRecord& op) {
  return sync(op, true);
}

DispatchResult PushHandler::reconcile(const store::OperationRecord& op) {
  return sync(op, false);
}

PrCreateHandler::PrCreateHandler(GithubHandlerContext context)
    : context_(std::move(context)) {}

DispatchResult PrCreateHandler::sync(const store::OperationRecord& op,
                                     bool performing) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const std::string head = input.value("head", "");
  if (head.empty()) {
    return failed("pr_create intent needs a head branch");
  }

  // One PR per head branch: look for ours before creating anything.
  auto existing =
      context_.client->list_open_pull_requests(context_.owner, context_.repo, head);
  if (!existing) {
    return classify(existing.error());
  }
  for (const auto& pr : *existing) {
    if (github::Client::body_has_marker(pr.body, op.op_id)) {
      // The PR exists; make sure labels and any ready-time reviewer request
      // from the intent landed too.
      auto labelled = apply_labels(context_, pr.number, input);
      if (!labelled) {
        return classify(labelled.error());
      }
      if (!input.value("draft", true) &&
          input.value("request_codeowners", false)) {
        auto reviewers = request_codeowners_reviewers(context_, pr.number);
        if (!reviewers) {
          return classify(reviewers.error());
        }
      }
      return confirmed(Json{{"number", pr.number}, {"html_url", pr.html_url}}.dump());
    }
  }
  if (!performing) {
    return retry("no pull request with this operation marker exists yet");
  }

  const std::string body = input.value("body", "") + "\n\n" +
                           github::Client::op_marker(op.op_id);
  auto created = context_.client->create_pull_request(
      context_.owner, context_.repo, input.value("title", ""), head,
      input.value("base", "main"), body, input.value("draft", true));
  if (!created) {
    return classify(created.error());
  }
  auto labelled = apply_labels(context_, created->number, input);
  if (!labelled) {
    return classify(labelled.error());
  }
  if (!input.value("draft", true) && input.value("request_codeowners", false)) {
    auto reviewers = request_codeowners_reviewers(context_, created->number);
    if (!reviewers) {
      return classify(reviewers.error());
    }
  }
  return confirmed(
      Json{{"number", created->number}, {"html_url", created->html_url}}.dump());
}

DispatchResult PrCreateHandler::perform(const store::OperationRecord& op) {
  return sync(op, true);
}

DispatchResult PrCreateHandler::reconcile(const store::OperationRecord& op) {
  return sync(op, false);
}

IssueCreateHandler::IssueCreateHandler(GithubHandlerContext context)
    : context_(std::move(context)) {}

DispatchResult IssueCreateHandler::sync(const store::OperationRecord& op,
                                        bool performing) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);

  auto existing =
      context_.client->list_issues(context_.owner, context_.repo, "all");
  if (!existing) {
    return classify(existing.error());
  }
  // Our own operation marker first, then the defect fingerprint (which makes
  // a recurring defect update the existing issue, design section 6.10).
  for (const auto& issue : *existing) {
    if (github::Client::body_has_marker(issue.body, op.op_id)) {
      return confirmed(Json{{"number", issue.number}, {"html_url", issue.html_url}}.dump());
    }
  }
  const std::string fingerprint = input.value("fingerprint", "");
  if (!fingerprint.empty()) {
    for (const auto& issue : *existing) {
      if (github::Client::body_has_fingerprint(issue.body, fingerprint)) {
        return confirmed(Json{{"number", issue.number}, {"html_url", issue.html_url}}.dump());
      }
    }
  }
  if (!performing) {
    return retry("no issue with this operation or fingerprint marker exists yet");
  }

  std::vector<std::string> labels;
  const auto labels_json = input.find("labels");
  if (labels_json != input.end() && labels_json->is_array()) {
    for (const auto& label : *labels_json) {
      labels.push_back(label.get<std::string>());
    }
  }
  std::string body =
      input.value("body", "") + "\n\n" + github::Client::op_marker(op.op_id);
  if (!fingerprint.empty()) {
    body += "\n" + github::Client::fingerprint_marker(fingerprint);
  }
  auto created = context_.client->create_issue(
      context_.owner, context_.repo, input.value("title", ""), body, labels);
  if (!created) {
    return classify(created.error());
  }
  return confirmed(
      Json{{"number", created->number}, {"html_url", created->html_url}}.dump());
}

DispatchResult IssueCreateHandler::perform(const store::OperationRecord& op) {
  return sync(op, true);
}

DispatchResult IssueCreateHandler::reconcile(const store::OperationRecord& op) {
  return sync(op, false);
}

PrUpdateHandler::PrUpdateHandler(GithubHandlerContext context)
    : context_(std::move(context)) {}

DispatchResult PrUpdateHandler::perform(const store::OperationRecord& op) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const int64_t number = input.value("number", int64_t{0});
  if (number == 0) {
    return failed("pr_update intent needs a pull request number");
  }
  const std::string body =
      input.value("body", "") + "\n\n" + github::Client::op_marker(op.op_id);
  const bool mark_ready = input.value("mark_ready", false);
  auto updated = context_.client->update_pull_request(
      context_.owner, context_.repo, number, body, mark_ready, input.value("state", ""));
  if (!updated) {
    return classify(updated.error());
  }
  auto labelled = apply_labels(context_, number, input);
  if (!labelled) {
    return classify(labelled.error());
  }
  if (mark_ready && input.value("request_codeowners", true)) {
    auto reviewers = request_codeowners_reviewers(context_, number);
    if (!reviewers) {
      return classify(reviewers.error());
    }
  }
  return confirmed(Json{{"number", number}, {"html_url", updated->html_url}}.dump());
}

DispatchResult PrUpdateHandler::reconcile(const store::OperationRecord& op) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const int64_t number = input.value("number", int64_t{0});
  auto pr = context_.client->get_pull_request(context_.owner, context_.repo, number);
  if (!pr) {
    return classify(pr.error());
  }
  if (github::Client::body_has_marker(pr->body, op.op_id)) {
    // The update landed; make sure the labels and the ready transition also
    // landed (all idempotent, so a repeat is harmless).
    auto labelled = apply_labels(context_, number, input);
    if (!labelled) {
      return classify(labelled.error());
    }
    if (input.value("mark_ready", false) && !pr->draft &&
        input.value("request_codeowners", true)) {
      auto reviewers = request_codeowners_reviewers(context_, number);
      if (!reviewers) {
        return classify(reviewers.error());
      }
    }
    return confirmed(Json{{"number", number}, {"html_url", pr->html_url}}.dump());
  }
  return retry("the update has not landed yet");
}

CommentHandler::CommentHandler(GithubHandlerContext context)
    : context_(std::move(context)) {}

DispatchResult CommentHandler::perform(const store::OperationRecord& op) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const int64_t number = input.value("issue_number", int64_t{0});
  if (number == 0) {
    return failed("comment intent needs an issue number");
  }
  const std::string body =
      input.value("body", "") + "\n\n" + github::Client::op_marker(op.op_id);
  const auto commented =
      context_.client->create_comment(context_.owner, context_.repo, number, body);
  if (!commented) {
    return classify(commented.error());
  }
  return confirmed(Json{{"issue_number", number}}.dump());
}

DispatchResult CommentHandler::reconcile(const store::OperationRecord& op) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const int64_t number = input.value("issue_number", int64_t{0});
  auto comments =
      context_.client->list_issue_comments(context_.owner, context_.repo, number);
  if (!comments) {
    return classify(comments.error());
  }
  // A stalled orchestrator's request can land after the takeover, so the
  // same operation may exist twice (design section 6.12.6 rule 4): keep the
  // earliest comment and delete the rest, with a note in the run log.
  std::vector<const github::Client::IssueComment*> matches;
  for (const auto& comment : *comments) {
    if (github::Client::body_has_marker(comment.body, op.op_id)) {
      matches.push_back(&comment);
    }
  }
  if (matches.empty()) {
    return retry("the comment has not landed yet");
  }
  for (std::size_t i = 1; i < matches.size(); ++i) {
    if (matches[i]->id == 0) {
      continue;  // no id, cannot address it for deletion
    }
    auto deleted = context_.client->delete_comment(context_.owner, context_.repo,
                                                   matches[i]->id);
    if (!deleted) {
      return retry("cannot delete duplicate comment " +
                   std::to_string(matches[i]->id) + ": " + deleted.error().message);
    }
    spdlog::warn("op {}: deleted duplicate comment {} on issue {} (late landing)",
                 op.op_id, matches[i]->id, number);
  }
  return confirmed(Json{{"issue_number", number}}.dump());
}

IssueUpdateHandler::IssueUpdateHandler(GithubHandlerContext context)
    : context_(std::move(context)) {}

DispatchResult IssueUpdateHandler::perform(const store::OperationRecord& op) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const int64_t number = input.value("number", int64_t{0});
  if (number == 0) {
    return failed("issue_update intent needs an issue number");
  }
  const std::string body =
      input.value("body", "") + "\n\n" + github::Client::op_marker(op.op_id);
  auto updated = context_.client->update_issue(context_.owner, context_.repo, number,
                                               body, input.value("state", ""));
  if (!updated) {
    return classify(updated.error());
  }
  return confirmed(Json{{"number", number}, {"state", updated->state}}.dump());
}

DispatchResult IssueUpdateHandler::reconcile(const store::OperationRecord& op) {
  if (context_.client == nullptr) {
    return failed("no GitHub client configured");
  }
  const Json input = parse_or_object(op.input_json);
  const int64_t number = input.value("number", int64_t{0});
  auto issue = context_.client->get_issue(context_.owner, context_.repo, number);
  if (!issue) {
    return classify(issue.error());
  }
  if (github::Client::body_has_marker(issue->body, op.op_id)) {
    return confirmed(Json{{"number", number}, {"state", issue->state}}.dump());
  }
  return retry("the issue update has not landed yet");
}

}  // namespace cf::orch
