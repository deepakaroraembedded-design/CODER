// cf-orch: the workspace milestone driver (design sections 6.6, 6.7, 6.13).
//
// The production MilestoneDriver: agent turns run through an injected
// AgentRunner (opencode in production), coder and QA work is snapshotted into
// git commits with the CF trailers, gates run through the staged gate runner
// on the snapshot commit, and integration squash-merges the milestone branch
// into the integration branch.
//
// Worktrees follow the design layout: wt/<run>/<milestone>/<role> on
// cf-local/<run>/<milestone>/<role>, with wt/<run>/integration on
// cf/<run>/integration.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "cf/common/process.hpp"
#include "cf/common/result.hpp"
#include "cf/gates/profile.hpp"
#include "cf/gates/runner.hpp"
#include "cf/git/repository.hpp"
#include "cf/orch/agent_runner.hpp"
#include "cf/orch/codeintel_sync.hpp"
#include "cf/orch/milestone_loop.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct DriverConfig {
  std::string run_id;
  std::filesystem::path workspace_root;
  std::string integration_branch;  // e.g. cf/R12/integration
  std::string base_branch = "main";
  gates::Profile profile;
  std::string profile_language;
  std::string profile_build_system;
  std::string profile_test_framework;
  // Host directory holding rules/semgrep; sandboxed gates mount it read-only.
  std::string rules_dir;
  std::chrono::milliseconds step_timeout{600000};
  // review.diff_threshold_lines (design section 15): a cumulative change
  // beyond this many lines since the last full review forces a full review
  // (design section 7.4).
  std::uint32_t full_review_threshold = 400;
  // Gate sandbox resource limits (design section 15, sandbox): 0 means no
  // limit.
  double gate_cpus = 0.0;
  std::uint64_t gate_memory_mb = 0;
  // When set, gates run inside a Docker sandbox using this image (design
  // section 6.8); empty runs them through the local executor.
  std::string sandbox_image;
  // When true, gates run in the daemon-free namespace sandbox (design section
  // 6.8) instead of Docker; takes precedence over sandbox_image.
  bool sandbox_namespaces = false;
  // Keeps the run's code intelligence index in step with the integration
  // branch (design section 6.5). Null: no indexing (no back end for the
  // plan's language). Not owned; must outlive the driver.
  CodeIntelSync* codeintel = nullptr;
};

class WorkspaceDriver final : public MilestoneDriver {
 public:
  WorkspaceDriver(plan::Plan plan, DriverConfig config, git::Repository& repository,
                  process::ProcessExecutor& executor, AgentRunner& agent_runner,
                  store::RunStore* store = nullptr);

  TurnResult run(MilestonePhase phase, std::string_view milestone_id) override;
  GateOutcome run_gates(std::string_view milestone_id) override;
  TurnResult run_test_dispute(std::string_view milestone_id) override;
  std::vector<std::string> integration_interfaces(
      std::string_view milestone_id) override;
  void mark_revalidating(std::string_view milestone_id) override;
  TurnResult run_test_repair(std::string_view milestone_id) override;
  SystemValidationResult validate_system() override;

  // Worktree layout helpers (also used by recovery and tests).
  std::filesystem::path worktree_for(std::string_view milestone_id,
                                     std::string_view role) const;
  std::filesystem::path integration_worktree() const;

  // Ensures the integration worktree exists and the code index reflects its
  // head (design section 6.5), so agents have symbol context from the first
  // turn. No-op when no index is configured.
  Status prepare_codeintel();

  // Base-freshness recovery (design section 10.4.9): rebase the integration
  // branch onto `upstream`; a conflict aborts the rebase and returns
  // GIT_MERGE_CONFLICT. `run_integration_gates` re-runs the trusted gate plan
  // on the rebased integration head, records the results for that commit and
  // applies waivers; it does not run a review.
  Status rebase_integration_onto(std::string_view upstream);
  Result<GateOutcome> run_integration_gates(std::string_view upstream);

 private:
  Result<std::filesystem::path> ensure_worktree(std::string_view milestone_id,
                                                std::string_view role);
  Result<std::filesystem::path> ensure_integration_worktree();
  // Records the pre-turn checkpoint (design section 6.12.3): the head of
  // every existing role worktree of the milestone and its stored state, so
  // recovery can reset a half-finished turn.
  void record_checkpoint(std::string_view milestone_id);
  Status snapshot(std::string_view milestone_id, std::string_view role,
                  const std::vector<std::string>& paths, const std::string& message,
                  const std::string& op_id);
  // The CF-Waivers trailer value for a milestone: "none" or the
  // comma-separated rule@scope list (design section 10.4.5).
  std::string waivers_trailer(std::string_view milestone_id) const;
  std::string start_point() const;
  AgentTurnRequest make_request(MilestonePhase phase, std::string_view milestone_id,
                                std::string_view role) const;

  plan::Plan plan_;
  DriverConfig config_;
  git::Repository& repository_;
  process::ProcessExecutor& executor_;
  AgentRunner& agent_runner_;
  store::RunStore* store_ = nullptr;
  // The integration head each milestone branched from, for the gate diff.
  std::map<std::string, std::string, std::less<>> base_sha_;
  // Milestones whose first gate run has already happened.
  std::map<std::string, bool, std::less<>> gated_once_;
  // Public interface paths changed by each milestone's integration (design
  // section 6.13.3).
  std::map<std::string, std::vector<std::string>, std::less<>> changed_interfaces_;
  // Milestones whose work is already merged into the integration branch.
  std::set<std::string> integrated_;
  // Done milestones being revalidated against a changed interface: their
  // gates, review and QA run against the integration worktree.
  std::set<std::string> revalidating_;
};

}  // namespace cf::orch
