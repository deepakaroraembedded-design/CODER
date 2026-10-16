// cf-orch: crash recovery (design section 6.12.4).
//
// `cf resume` runs the recovery procedure: take the run lease, release locks
// left by an older epoch, reconcile every operation left in flight (STARTED
// past the in-flight grace period, or NEEDS_RECONCILE), reset each worktree
// to its latest checkpoint (saving the differences for audit), and re-derive
// every milestone state from the store.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/git/repository.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct RecoveryOptions {
  std::string holder;  // this orchestrator's identity
  std::chrono::seconds lease_ttl{30};
  // In-flight grace period (design section 6.12.6 rule 3): operations left
  // STARTED by an older epoch are only reconciled once this has passed.
  std::chrono::seconds in_flight_grace{120};
};

struct RecoveryReport {
  uint64_t epoch = 0;
  std::size_t locks_released = 0;
  std::size_t started_reconciled = 0;
  std::size_t needs_reconcile_retried = 0;
  std::vector<std::string> still_needs_reconcile;
  std::size_t worktrees_reset = 0;
  std::vector<store::MilestoneRecord> milestones;

  std::string to_json() const;
};

// Worktree reset (design section 6.12.4 step 4). The git-backed
// implementation lives below; tests inject a fake.
class RecoveryHook {
 public:
  virtual ~RecoveryHook() = default;
  virtual Status reset_worktrees(const store::CheckpointRecord& checkpoint) = 0;
};

class Recovery {
 public:
  // `hook` is optional: without it, worktrees are not touched.
  explicit Recovery(store::RunStore& store, std::unique_ptr<RecoveryHook> hook = nullptr);

  Result<RecoveryReport> resume(std::string_view run_id, Dispatcher& dispatcher,
                                const RecoveryOptions& options, store::UnixTime now);

 private:
  store::RunStore& store_;
  std::unique_ptr<RecoveryHook> hook_;
};

// Resets the worktrees recorded in a checkpoint to their checkpoint commits.
// Work that differs is parked on refs/cf/recovered/<run>/<at>/<role> first.
class GitWorktreeHook final : public RecoveryHook {
 public:
  GitWorktreeHook(git::Repository& repository, std::filesystem::path workspace_root);

  Status reset_worktrees(const store::CheckpointRecord& checkpoint) override;

 private:
  git::Repository& repository_;
  std::filesystem::path workspace_root_;
};

}  // namespace cf::orch
