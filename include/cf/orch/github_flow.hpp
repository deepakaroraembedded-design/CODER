// cf-orch: the GitHub flow (design section 10.4.4).
//
// After each orchestrator step, syncs the run's GitHub state: every milestone
// that reached Done is pushed to `cf/<run>/<milestone>` and folded into
// `cf/<run>/integration`; a draft pull request from integration to the base
// branch is opened on the first milestone and updated afterwards; the run
// reaching Done marks it ready for review. Every effect goes through the
// intent-and-reconcile protocol (idempotency keys derived from the content).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cf/git/repository.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/orch/github_handlers.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct GithubFlowConfig {
  std::string owner;
  std::string repo;
  std::string remote = "origin";
  std::string base_branch = "main";
  std::string title_prefix = "CF";
};

class GithubFlow {
 public:
  GithubFlow(store::RunStore& store, Dispatcher& dispatcher,
             git::Repository& repository, GithubFlowConfig config);

  // Records (idempotently) and dispatches the operations needed for the
  // current milestone states and run state. Returns the number of operations
  // dispatched.
  Result<std::size_t> sync(std::string_view run_id, const plan::Plan& plan,
                           RunState run_state, uint64_t epoch, store::UnixTime now);

 private:
  // Records a push intent for a branch when its content has not been intended
  // before.
  Status ensure_push(std::string_view run_id, std::string_view milestone_id,
                     const std::string& branch, const std::string& ref,
                     store::UnixTime now);

  Status ensure_pr(std::string_view run_id, const plan::Plan& plan,
                   RunState run_state, store::UnixTime now);

  // Escalation: push the work in progress to cf/<run>/escalation/<at> and
  // comment on the draft PR (design section 10.4.4).
  Status ensure_escalation(std::string_view run_id, store::UnixTime now);

  // Abort: close the draft PR with a comment.
  Status ensure_abort(std::string_view run_id, const plan::Plan& plan,
                      RunState run_state, store::UnixTime now);

  // The number of the confirmed pull request, or 0.
  Result<int64_t> confirmed_pr_number(std::string_view run_id) const;

  // The timestamp of the last transition into the given state, or 0.
  Result<store::UnixTime> last_transition_at(std::string_view run_id,
                                             std::string_view to_state) const;

  std::string build_pr_body(std::string_view run_id, const plan::Plan& plan,
                            RunState run_state) const;

  store::RunStore& store_;
  Dispatcher& dispatcher_;
  git::Repository& repository_;
  GithubFlowConfig config_;
};

}  // namespace cf::orch
