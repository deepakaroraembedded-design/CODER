#include "cf/orch/state_machine.hpp"

#include <array>
#include <utility>

namespace cf::orch {
namespace {

struct RunTransition {
  RunState from;
  RunEvent event;
  RunState to;
};

// Design section 7.1 (the run-level state diagram).
constexpr std::array<RunTransition, 14> kRunTransitions = {{
    {RunState::Created, RunEvent::Start, RunState::Ingesting},
    {RunState::Ingesting, RunEvent::IndexReady, RunState::Refining},
    {RunState::Refining, RunEvent::DesignPackageReady, RunState::AwaitingDesignApproval},
    {RunState::AwaitingDesignApproval, RunEvent::ChangesRequested, RunState::Refining},
    {RunState::AwaitingDesignApproval, RunEvent::Approved, RunState::Executing},
    {RunState::AwaitingDesignApproval, RunEvent::ApprovalDeadlineExpired, RunState::Aborted},
    {RunState::Executing, RunEvent::AllMilestonesDone, RunState::SystemValidation},
    {RunState::SystemValidation, RunEvent::SystemDefectsFiled, RunState::Executing},
    {RunState::SystemValidation, RunEvent::SystemValidationPassed, RunState::Done},
    {RunState::Executing, RunEvent::BudgetExceeded, RunState::Escalated},
    {RunState::Executing, RunEvent::NoProgress, RunState::Escalated},
    {RunState::Escalated, RunEvent::HumanResolved, RunState::Executing},
    {RunState::Escalated, RunEvent::HumanAborted, RunState::Aborted},
    {RunState::Escalated, RunEvent::EscalationDeadlineExpired, RunState::Aborted},
}};

struct MilestoneTransition {
  MilestoneState from;
  MilestoneEvent event;
  MilestoneState to;
};

// Design section 7.2 (the milestone loop). Pending and Blocked come from
// design section 6.13.2; Escalated -> Fixing is the "human resolves" edge
// analogous to Escalated -> Executing at run level. StallOrBudget escalates
// from every active state, per the loop-control limits in design section 12
// (the diagram shows the Fixing edge as an example).
constexpr std::array<MilestoneTransition, 26> kMilestoneTransitions = {{
    {MilestoneState::Pending, MilestoneEvent::Start, MilestoneState::TestAuthoring},
    {MilestoneState::Pending, MilestoneEvent::Block, MilestoneState::Blocked},
    {MilestoneState::Blocked, MilestoneEvent::Unblock, MilestoneState::TestAuthoring},
    {MilestoneState::TestAuthoring, MilestoneEvent::TestsCommitted, MilestoneState::Coding},
    {MilestoneState::Coding, MilestoneEvent::WorkSubmitted, MilestoneState::Gating},
    {MilestoneState::Gating, MilestoneEvent::GatesFailed, MilestoneState::Fixing},
    {MilestoneState::Gating, MilestoneEvent::GatesPassed, MilestoneState::Reviewing},
    {MilestoneState::Gating, MilestoneEvent::GatesPassedReviewValid,
     MilestoneState::QaValidating},
    {MilestoneState::Reviewing, MilestoneEvent::ChangesRequested, MilestoneState::Fixing},
    {MilestoneState::Reviewing, MilestoneEvent::Approved, MilestoneState::QaValidating},
    {MilestoneState::QaValidating, MilestoneEvent::DefectsFiled, MilestoneState::Fixing},
    {MilestoneState::QaValidating, MilestoneEvent::ExitCriteriaMet,
     MilestoneState::Integrating},
    {MilestoneState::Fixing, MilestoneEvent::WorkSubmitted, MilestoneState::Gating},
    {MilestoneState::Fixing, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::Integrating, MilestoneEvent::Merged, MilestoneState::Done},
    {MilestoneState::Integrating, MilestoneEvent::RebaseConflictOrRegateFailed,
     MilestoneState::Fixing},
    {MilestoneState::Done, MilestoneEvent::InterfaceChanged, MilestoneState::Revalidating},
    {MilestoneState::Revalidating, MilestoneEvent::GatesRerun, MilestoneState::Gating},
    {MilestoneState::Escalated, MilestoneEvent::HumanResolved, MilestoneState::Fixing},
    {MilestoneState::TestAuthoring, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::Coding, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::Gating, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::Reviewing, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::QaValidating, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::Integrating, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
    {MilestoneState::Revalidating, MilestoneEvent::StallOrBudget, MilestoneState::Escalated},
}};

std::optional<RunState> next_run_state(RunState from, RunEvent event) {
  for (const auto& transition : kRunTransitions) {
    if (transition.from == from && transition.event == event) {
      return transition.to;
    }
  }
  return std::nullopt;
}

std::optional<MilestoneState> next_milestone_state(MilestoneState from,
                                                   MilestoneEvent event) {
  for (const auto& transition : kMilestoneTransitions) {
    if (transition.from == from && transition.event == event) {
      return transition.to;
    }
  }
  return std::nullopt;
}

std::string event_json(RunEvent event) {
  return "{\"event\":\"" + std::string(to_string(event)) + "\"}";
}

}  // namespace

std::string_view to_string(RunState state) {
  switch (state) {
    case RunState::Created: return "Created";
    case RunState::Ingesting: return "Ingesting";
    case RunState::Refining: return "Refining";
    case RunState::AwaitingDesignApproval: return "AwaitingDesignApproval";
    case RunState::Executing: return "Executing";
    case RunState::SystemValidation: return "SystemValidation";
    case RunState::Done: return "Done";
    case RunState::Escalated: return "Escalated";
    case RunState::Aborted: return "Aborted";
  }
  return "Unknown";
}

std::string_view to_string(RunEvent event) {
  switch (event) {
    case RunEvent::Start: return "start";
    case RunEvent::IndexReady: return "index_ready";
    case RunEvent::DesignPackageReady: return "design_package_ready";
    case RunEvent::ChangesRequested: return "changes_requested";
    case RunEvent::Approved: return "approved";
    case RunEvent::AllMilestonesDone: return "all_milestones_done";
    case RunEvent::SystemDefectsFiled: return "system_defects_filed";
    case RunEvent::SystemValidationPassed: return "system_validation_passed";
    case RunEvent::BudgetExceeded: return "budget_exceeded";
    case RunEvent::NoProgress: return "no_progress";
    case RunEvent::HumanResolved: return "human_resolved";
    case RunEvent::HumanAborted: return "human_aborted";
    case RunEvent::ApprovalDeadlineExpired: return "approval_deadline_expired";
    case RunEvent::EscalationDeadlineExpired: return "escalation_deadline_expired";
  }
  return "unknown";
}

std::string_view to_string(MilestoneState state) {
  switch (state) {
    case MilestoneState::Pending: return "Pending";
    case MilestoneState::Blocked: return "Blocked";
    case MilestoneState::TestAuthoring: return "TestAuthoring";
    case MilestoneState::Coding: return "Coding";
    case MilestoneState::Gating: return "Gating";
    case MilestoneState::Reviewing: return "Reviewing";
    case MilestoneState::QaValidating: return "QaValidating";
    case MilestoneState::Fixing: return "Fixing";
    case MilestoneState::Integrating: return "Integrating";
    case MilestoneState::Revalidating: return "Revalidating";
    case MilestoneState::Done: return "Done";
    case MilestoneState::Escalated: return "Escalated";
  }
  return "Unknown";
}

std::string_view to_string(MilestoneEvent event) {
  switch (event) {
    case MilestoneEvent::Start: return "start";
    case MilestoneEvent::Block: return "block";
    case MilestoneEvent::Unblock: return "unblock";
    case MilestoneEvent::TestsCommitted: return "tests_committed";
    case MilestoneEvent::WorkSubmitted: return "work_submitted";
    case MilestoneEvent::GatesFailed: return "gates_failed";
    case MilestoneEvent::GatesPassed: return "gates_passed";
    case MilestoneEvent::GatesPassedReviewValid: return "gates_passed_review_valid";
    case MilestoneEvent::ChangesRequested: return "changes_requested";
    case MilestoneEvent::Approved: return "approved";
    case MilestoneEvent::DefectsFiled: return "defects_filed";
    case MilestoneEvent::ExitCriteriaMet: return "exit_criteria_met";
    case MilestoneEvent::StallOrBudget: return "stall_or_budget";
    case MilestoneEvent::Merged: return "merged";
    case MilestoneEvent::RebaseConflictOrRegateFailed: return "rebase_conflict_or_regate_failed";
    case MilestoneEvent::InterfaceChanged: return "interface_changed";
    case MilestoneEvent::GatesRerun: return "gates_rerun";
    case MilestoneEvent::HumanResolved: return "human_resolved";
  }
  return "unknown";
}

std::optional<RunState> run_state_from_string(std::string_view text) {
  for (const RunState state :
       {RunState::Created, RunState::Ingesting, RunState::Refining,
        RunState::AwaitingDesignApproval, RunState::Executing, RunState::SystemValidation,
        RunState::Done, RunState::Escalated, RunState::Aborted}) {
    if (to_string(state) == text) {
      return state;
    }
  }
  return std::nullopt;
}

std::optional<MilestoneState> milestone_state_from_string(std::string_view text) {
  for (const MilestoneState state :
       {MilestoneState::Pending, MilestoneState::Blocked, MilestoneState::TestAuthoring,
        MilestoneState::Coding, MilestoneState::Gating, MilestoneState::Reviewing,
        MilestoneState::QaValidating, MilestoneState::Fixing, MilestoneState::Integrating,
        MilestoneState::Revalidating, MilestoneState::Done, MilestoneState::Escalated}) {
    if (to_string(state) == text) {
      return state;
    }
  }
  return std::nullopt;
}

bool is_active(MilestoneState state) {
  switch (state) {
    case MilestoneState::TestAuthoring:
    case MilestoneState::Coding:
    case MilestoneState::Gating:
    case MilestoneState::Reviewing:
    case MilestoneState::QaValidating:
    case MilestoneState::Fixing:
    case MilestoneState::Integrating:
    case MilestoneState::Revalidating:
      return true;
    case MilestoneState::Pending:
    case MilestoneState::Blocked:
    case MilestoneState::Done:
    case MilestoneState::Escalated:
      return false;
  }
  return false;
}

StateMachine::StateMachine(store::RunStore& store, std::string run_id)
    : store_(store), run_id_(std::move(run_id)) {}

Status StateMachine::register_milestone(std::string_view milestone_id) {
  const auto existing = store_.get_milestone(run_id_, milestone_id);
  if (existing.has_value()) {
    return fail(error_codes::kMilestoneExists,
                "milestone already registered: " + std::string(milestone_id));
  }
  store::MilestoneRecord milestone;
  milestone.run_id = run_id_;
  milestone.milestone_id = std::string(milestone_id);
  milestone.state = to_string(MilestoneState::Pending);
  milestone.iterations = 0;
  return store_.upsert_milestone(milestone);
}

Result<RunState> StateMachine::apply(RunEvent event,
                                     const std::vector<store::NewOperation>& intents,
                                     store::UnixTime now) {
  const auto run = store_.get_run(run_id_);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  const auto from = run_state_from_string(run->state);
  if (!from) {
    return fail(error_codes::kInvalidRunTransition,
                "run " + run_id_ + " has unknown stored state '" + run->state + "'");
  }
  const auto to = next_run_state(*from, event);
  if (!to) {
    return fail(error_codes::kInvalidRunTransition,
                "run event '" + std::string(to_string(event)) + "' is not valid in state " +
                    std::string(to_string(*from)));
  }
  const auto applied = store_.apply_transition(run_id_, run->state, to_string(*to),
                                               event_json(event), intents, now);
  if (!applied) {
    return tl::make_unexpected(applied.error());
  }
  return *to;
}

Result<MilestoneState> StateMachine::apply(std::string_view milestone_id,
                                           MilestoneEvent event, store::UnixTime now) {
  const auto milestone = store_.get_milestone(run_id_, milestone_id);
  if (!milestone) {
    return fail(error_codes::kMilestoneNotFound,
                "milestone not registered: " + std::string(milestone_id));
  }
  const auto from = milestone_state_from_string(milestone->state);
  if (!from) {
    return fail(error_codes::kInvalidMilestoneTransition,
                "milestone " + std::string(milestone_id) + " has unknown stored state '" +
                    milestone->state + "'");
  }
  const auto to = next_milestone_state(*from, event);
  if (!to) {
    return fail(error_codes::kInvalidMilestoneTransition,
                "milestone event '" + std::string(to_string(event)) +
                    "' is not valid in state " + std::string(to_string(*from)));
  }
  const uint32_t iterations =
      milestone->iterations + (event == MilestoneEvent::WorkSubmitted ? 1 : 0);
  const auto stored = store_.set_milestone_state(run_id_, milestone_id, to_string(*to),
                                                 iterations, now);
  if (!stored) {
    return tl::make_unexpected(stored.error());
  }
  return *to;
}

RunState StateMachine::current() const {
  const auto run = store_.get_run(run_id_);
  if (!run) {
    return RunState::Created;
  }
  return run_state_from_string(run->state).value_or(RunState::Created);
}

Result<MilestoneState> StateMachine::current(std::string_view milestone_id) const {
  const auto milestone = store_.get_milestone(run_id_, milestone_id);
  if (!milestone) {
    return fail(error_codes::kMilestoneNotFound,
                "milestone not registered: " + std::string(milestone_id));
  }
  const auto state = milestone_state_from_string(milestone->state);
  if (!state) {
    return fail(error_codes::kInvalidMilestoneTransition,
                "milestone " + std::string(milestone_id) + " has unknown stored state '" +
                    milestone->state + "'");
  }
  return *state;
}

}  // namespace cf::orch
