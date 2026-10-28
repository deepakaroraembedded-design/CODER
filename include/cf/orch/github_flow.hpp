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
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/git/repository.hpp"
#include "cf/github/pre_push.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/orch/github_handlers.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

namespace github_error_codes {
inline constexpr const char* kPrePushRejected = "ORCH_PRE_PUSH_REJECTED";
}  // namespace github_error_codes

struct GithubFlowConfig {
  std::string owner;
  std::string repo;
  std::string remote = "origin";
  std::string base_branch = "main";
  std::string title_prefix = "CF";
  // Runs on the exact commit before any push (design section 10.4.6).
  github::PrePushConfig pre_push;
  // When the flow pushes (design sections 10.4.4, 15). A disabled event
  // performs no push (and no escalation comment). The PR is marked ready
  // only when run_done is enabled.
  bool push_on_milestone_done = true;
  bool push_on_run_done = true;
  bool push_on_escalation = true;
  // Pull-request policy (design sections 10.4.4, 10.4.8): when
  // draft_until_done is false the PR opens ready; labels are applied to the
  // PR; request_codeowners controls the ready-time reviewer request.
  bool pr_draft_until_done = true;
  std::vector<std::string> pr_labels;
  bool pr_request_codeowners = true;
  // Path shown in the PR body for the full run report (design section
  // 10.4.8); empty omits the link.
  std::string report_path;
  // Run directory holding usage-<role>.jsonl; empty omits agent usage from
  // the summary.
  std::filesystem::path run_dir;
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

  Result<std::string> build_pr_body(std::string_view run_id,
                                    const plan::Plan& plan,
                                    RunState run_state) const;

  store::RunStore& store_;
  Dispatcher& dispatcher_;
  git::Repository& repository_;
  GithubFlowConfig config_;
};

}  // namespace cf::orch
