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
#include <string>
#include <string_view>

#include "cf/common/process.hpp"
#include "cf/common/result.hpp"
#include "cf/gates/profile.hpp"
#include "cf/gates/runner.hpp"
#include "cf/git/repository.hpp"
#include "cf/orch/agent_runner.hpp"
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
  std::chrono::milliseconds step_timeout{600000};
  // When set, gates run inside a Docker sandbox using this image (design
  // section 6.8); empty runs them through the local executor.
  std::string sandbox_image;
  // When true, gates run in the daemon-free namespace sandbox (design section
  // 6.8) instead of Docker; takes precedence over sandbox_image.
  bool sandbox_namespaces = false;
};

class WorkspaceDriver final : public MilestoneDriver {
 public:
  WorkspaceDriver(plan::Plan plan, DriverConfig config, git::Repository& repository,
                  process::ProcessExecutor& executor, AgentRunner& agent_runner,
                  store::RunStore* store = nullptr);

  TurnResult run(MilestonePhase phase, std::string_view milestone_id) override;
  GateOutcome run_gates(std::string_view milestone_id) override;
  TurnResult run_test_dispute(std::string_view milestone_id) override;
  SystemValidationResult validate_system() override;

  // Worktree layout helpers (also used by recovery and tests).
  std::filesystem::path worktree_for(std::string_view milestone_id,
                                     std::string_view role) const;
  std::filesystem::path integration_worktree() const;

 private:
  Result<std::filesystem::path> ensure_worktree(std::string_view milestone_id,
                                                std::string_view role);
  Status snapshot(std::string_view milestone_id, std::string_view role,
                  const std::vector<std::string>& paths, const std::string& message,
                  const std::string& op_id);
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
};

}  // namespace cf::orch
