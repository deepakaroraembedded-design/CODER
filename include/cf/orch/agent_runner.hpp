// cf-orch: agent turn interface (design section 6.6).
//
// The driver asks an AgentRunner for one turn per phase. The production
// implementation drives opencode through the cf-agents adapter and collects
// the structured MCP submission; tests inject a scripted runner.
#pragma once

#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/orch/milestone_loop.hpp"

namespace cf::orch {

struct AgentTurnRequest {
  MilestonePhase phase = MilestonePhase::Code;
  std::string run_id;
  std::string milestone_id;
  std::string role;       // coder | reviewer | qa
  std::string worktree;   // the role's worktree
  std::vector<std::string> requirement_ids;
  std::vector<std::string> open_defect_ids;
  std::string brief;      // the task, filled into the role prompt
};

struct AgentTurnOutcome {
  bool completed = false;  // the agent ended with its structured submission
  std::string summary;
  std::string detail;
  bool changes_requested = false;  // Review: verdict CHANGES_REQUESTED
  bool exit_criteria_met = false;  // Validate: all tests pass, criteria met
  // Code/Fix: the turn ended with dispute_test; the orchestrator routes the
  // dispute to QA before the next gate run (design sections 6.9, 12).
  std::string disputed_test_id;
};

class AgentRunner {
 public:
  virtual ~AgentRunner() = default;

  virtual Result<AgentTurnOutcome> run_turn(const AgentTurnRequest& request) = 0;

  // Run-level system validation (design section 11.2). The default reports
  // that the runner does not support it.
  virtual Result<AgentTurnOutcome> run_system_validation(
      const AgentTurnRequest& request) {
    (void)request;
    return AgentTurnOutcome{false, "", "system validation is not supported by this runner",
                            false, false, ""};
  }
};

}  // namespace cf::orch
