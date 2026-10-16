// cf-orch: run-level orchestrator (design sections 6.1, 7.1, 12).
//
// Ties the pieces of a run together deterministically: the dependency-aware
// scheduler starts milestones, each active milestone is driven by its
// MilestoneLoop, the run budget is checked against the LLM call ledger, and
// the run moves to system validation when every milestone is Done. When
// nothing can progress or the budget is exceeded, the run escalates and an
// escalation bundle is written (design section 12).
//
// The single-threaded skeleton is the deterministic core the real process
// (Boost.Asio strands per milestone) will drive; the MilestoneDriver remains
// the only integration point with the agents, git and the gate runner.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cf/common/result.hpp"
#include "cf/orch/milestone_loop.hpp"
#include "cf/orch/scheduler.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct OrchestratorConfig {
  SchedulerConfig scheduler;
  LoopLimits loop_limits;
  // Run budget (design section 8.1); the ledger is checked after every
  // milestone turn and gate run.
  std::optional<double> max_usd_total;
  std::optional<uint64_t> max_tokens_total;
  // Empty: escalation bundles are not written.
  std::string escalation_dir;
};

class RunOrchestrator {
 public:
  RunOrchestrator(store::RunStore& store, std::string run_id, const plan::Plan& plan,
                  MilestoneDriver& driver, OrchestratorConfig config = {});

  // Advances the run as far as it can with the current information: schedules
  // milestones, drives their loops, checks the budget, and enters system
  // validation when every milestone is Done. States that wait on people
  // (Created, Ingesting, Refining, AwaitingDesignApproval) or are terminal
  // are returned unchanged.
  Result<RunState> step(store::UnixTime now);

  const std::string& last_escalation_reason() const { return last_escalation_reason_; }

 private:
  Result<RunState> escalate(std::string_view reason, RunEvent event, store::UnixTime now);
  Result<std::optional<std::string>> budget_exceeded() const;
  Result<bool> all_milestones_done() const;
  // Registers every plan milestone that has no row yet, in Pending.
  Status ensure_milestones_registered();

  store::RunStore& store_;
  std::string run_id_;
  const plan::Plan& plan_;
  MilestoneDriver& driver_;
  OrchestratorConfig config_;
  StateMachine machine_;
  std::string last_escalation_reason_;
};

}  // namespace cf::orch
