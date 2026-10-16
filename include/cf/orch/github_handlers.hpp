// cf-orch: GitHub operation handlers (design sections 6.12, 10.4).
//
// Push, pull-request and issue operations follow the intent-and-reconcile
// protocol: the intent is already committed when the handler runs; the
// handler performs the effect and every handler is also its own reconciler
// (idempotent detection of a prior effect, so a retry after a crash is safe).
#pragma once

#include <string>

#include "cf/git/repository.hpp"
#include "cf/github/client.hpp"
#include "cf/orch/dispatcher.hpp"

namespace cf::orch {

struct GithubHandlerContext {
  git::Repository* repository = nullptr;
  github::Client* client = nullptr;
  std::string owner;
  std::string repo;
  std::string remote = "origin";
};

// Operation kind "push"; input: {remote, branch, commit}, expected: {old, new}.
class PushHandler final : public OperationHandler {
 public:
  explicit PushHandler(GithubHandlerContext context);

  std::string_view kind() const override { return "push"; }
  DispatchResult perform(const store::OperationRecord& op) override;
  DispatchResult reconcile(const store::OperationRecord& op) override;

 private:
  DispatchResult sync(const store::OperationRecord& op, bool performing);

  GithubHandlerContext context_;
};

// Operation kind "pr_create"; input: {base, head, title, body, draft}.
class PrCreateHandler final : public OperationHandler {
 public:
  explicit PrCreateHandler(GithubHandlerContext context);

  std::string_view kind() const override { return "pr_create"; }
  DispatchResult perform(const store::OperationRecord& op) override;
  DispatchResult reconcile(const store::OperationRecord& op) override;

 private:
  DispatchResult sync(const store::OperationRecord& op, bool performing);

  GithubHandlerContext context_;
};

// Operation kind "issue_create"; input: {title, body, labels, fingerprint?}.
// The fingerprint marker de-duplicates a recurring defect across runs (design
// section 6.10): a matching issue is reused instead of opening a second one.
class IssueCreateHandler final : public OperationHandler {
 public:
  explicit IssueCreateHandler(GithubHandlerContext context);

  std::string_view kind() const override { return "issue_create"; }
  DispatchResult perform(const store::OperationRecord& op) override;
  DispatchResult reconcile(const store::OperationRecord& op) override;

 private:
  DispatchResult sync(const store::OperationRecord& op, bool performing);

  GithubHandlerContext context_;
};

// Operation kind "issue_update"; input: {number, body, state}. Used for the
// defect status sync (close on VERIFIED, reopen on REOPENED).
class IssueUpdateHandler final : public OperationHandler {
 public:
  explicit IssueUpdateHandler(GithubHandlerContext context);

  std::string_view kind() const override { return "issue_update"; }
  DispatchResult perform(const store::OperationRecord& op) override;
  DispatchResult reconcile(const store::OperationRecord& op) override;

 private:
  GithubHandlerContext context_;
};

// Operation kind "pr_update"; input: {number, body, mark_ready}. Each update
// writes the complete intended state, stamped with the op marker (design
// section 6.12.6).
class PrUpdateHandler final : public OperationHandler {
 public:
  explicit PrUpdateHandler(GithubHandlerContext context);

  std::string_view kind() const override { return "pr_update"; }
  DispatchResult perform(const store::OperationRecord& op) override;
  DispatchResult reconcile(const store::OperationRecord& op) override;

 private:
  GithubHandlerContext context_;
};

// Operation kind "comment"; input: {issue_number, body}.
class CommentHandler final : public OperationHandler {
 public:
  explicit CommentHandler(GithubHandlerContext context);

  std::string_view kind() const override { return "comment"; }
  DispatchResult perform(const store::OperationRecord& op) override;
  DispatchResult reconcile(const store::OperationRecord& op) override;

 private:
  GithubHandlerContext context_;
};

}  // namespace cf::orch
