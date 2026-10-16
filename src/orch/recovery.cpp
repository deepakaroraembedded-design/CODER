#include "cf/orch/recovery.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace cf::orch {
namespace {

using Json = nlohmann::json;

}  // namespace

std::string RecoveryReport::to_json() const {
  Json json_milestones = Json::array();
  for (const auto& milestone : milestones) {
    json_milestones.push_back(Json{{"id", milestone.milestone_id},
                                   {"state", milestone.state},
                                   {"iterations", milestone.iterations}});
  }
  return Json{{"epoch", epoch},
              {"locks_released", locks_released},
              {"started_reconciled", started_reconciled},
              {"needs_reconcile_retried", needs_reconcile_retried},
              {"still_needs_reconcile", still_needs_reconcile},
              {"worktrees_reset", worktrees_reset},
              {"milestones", std::move(json_milestones)}}
      .dump(2);
}

Recovery::Recovery(store::RunStore& store, std::unique_ptr<RecoveryHook> hook)
    : store_(store), hook_(std::move(hook)) {}

Result<RecoveryReport> Recovery::resume(std::string_view run_id, Dispatcher& dispatcher,
                                        const RecoveryOptions& options,
                                        store::UnixTime now) {
  RecoveryReport report;

  // 1. Take the run lease; a live lease held elsewhere stops recovery.
  auto lease = store_.acquire_lease(run_id, options.holder, options.lease_ttl, now);
  if (!lease) {
    return tl::make_unexpected(lease.error());
  }
  report.epoch = lease->epoch;

  // Locks are held under the run lease; anything from an older epoch is
  // released so milestones can re-acquire them (design section 6.13.2).
  auto released = store_.release_stale_locks(run_id, lease->epoch);
  if (!released) {
    return tl::make_unexpected(released.error());
  }
  report.locks_released = *released;

  // 2. Reconcile operations left STARTED by an older epoch, past the
  //    in-flight grace period (design section 6.12.6 rule 3).
  auto started = dispatcher.reconcile_stale(lease->epoch, options.in_flight_grace, now);
  if (!started) {
    return tl::make_unexpected(started.error());
  }
  report.started_reconciled = *started;

  // 3. Retry every operation that needs reconciliation.
  auto needs = store_.list_operations(run_id, {store::op_state::kNeedsReconcile});
  if (!needs) {
    return tl::make_unexpected(needs.error());
  }
  for (const auto& operation : *needs) {
    auto outcome = dispatcher.reconcile_one(operation.op_id, lease->epoch, now);
    if (!outcome) {
      return tl::make_unexpected(outcome.error());
    }
    ++report.needs_reconcile_retried;
  }
  needs = store_.list_operations(run_id, {store::op_state::kNeedsReconcile});
  if (!needs) {
    return tl::make_unexpected(needs.error());
  }
  for (const auto& operation : *needs) {
    report.still_needs_reconcile.push_back(operation.op_id);
  }

  // 4. Reset every worktree to its latest checkpoint.
  auto milestones = store_.list_milestones(run_id);
  if (!milestones) {
    return tl::make_unexpected(milestones.error());
  }
  if (hook_ != nullptr) {
    for (const auto& milestone : *milestones) {
      auto checkpoint = store_.latest_checkpoint(run_id, milestone.milestone_id);
      if (checkpoint.has_value()) {
        auto reset = hook_->reset_worktrees(*checkpoint);
        if (!reset) {
          return tl::make_unexpected(reset.error());
        }
        ++report.worktrees_reset;
      } else if (checkpoint.error().code != store::error_codes::kNotFound) {
        return tl::make_unexpected(checkpoint.error());
      }
    }
  }

  // 5. Re-derive every milestone state from the store.
  for (const auto& milestone : *milestones) {
    if (!milestone_state_from_string(milestone.state).has_value()) {
      return fail(error_codes::kInvalidMilestoneTransition,
                  "milestone " + milestone.milestone_id + " has unknown stored state '" +
                      milestone.state + "'");
    }
  }
  report.milestones = *milestones;
  return report;
}

GitWorktreeHook::GitWorktreeHook(git::Repository& repository,
                                 std::filesystem::path workspace_root)
    : repository_(repository), workspace_root_(std::move(workspace_root)) {}

Status GitWorktreeHook::reset_worktrees(const store::CheckpointRecord& checkpoint) {
  const Json heads = Json::parse(checkpoint.worktree_heads_json, nullptr, false);
  if (heads.is_discarded() || !heads.is_object()) {
    return {};  // no worktree heads recorded: nothing to reset
  }
  for (auto it = heads.begin(); it != heads.end(); ++it) {
    const std::string role = it.key();
    if (!it.value().is_string()) {
      continue;
    }
    const std::string checkpoint_sha = it.value().get<std::string>();
    const std::filesystem::path worktree = workspace_root_ / "wt" / checkpoint.run_id /
                                           checkpoint.milestone_id / role;
    std::error_code ec;
    if (!std::filesystem::is_directory(worktree, ec)) {
      continue;  // worktree already gone
    }

    auto dirty = repository_.is_dirty(worktree);
    if (!dirty) {
      return tl::make_unexpected(dirty.error());
    }
    auto head = repository_.head_sha(worktree);
    if (!head) {
      return tl::make_unexpected(head.error());
    }
    if (!*dirty && *head == checkpoint_sha) {
      continue;  // already at the checkpoint
    }

    if (*dirty) {
      // Park the partial edits as a commit so they stay reachable.
      git::CommitTrailers trailers;
      trailers.run_id = checkpoint.run_id;
      trailers.milestone_id = checkpoint.milestone_id;
      trailers.op_id = "recovery";
      auto committed = repository_.commit_paths(
          worktree, {"."},
          "cf-recovered: partial work from " + role + " at " +
              std::to_string(checkpoint.at),
          trailers);
      if (!committed) {
        return tl::make_unexpected(committed.error());
      }
      head = committed;
    }
    auto saved = repository_.update_ref(
        "refs/cf/recovered/" + checkpoint.run_id + "/" +
            std::to_string(checkpoint.at) + "/" + role,
        *head);
    if (!saved) {
      return tl::make_unexpected(saved.error());
    }
    auto reset = repository_.reset_hard(worktree, checkpoint_sha);
    if (!reset) {
      return tl::make_unexpected(reset.error());
    }
  }
  return {};
}

}  // namespace cf::orch
