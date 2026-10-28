// cf-orch: run and milestone state machines (design section 7).
//
// The orchestrator is a deterministic state machine (D6): every transition is
// validated against the diagrams in design sections 7.1 and 7.2, persisted in
// the run store together with any operation intents, and can be re-derived
// from the store after a crash. State is always read from the store, never
// cached in memory.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kInvalidRunTransition = "ORCH_INVALID_RUN_TRANSITION";
inline constexpr const char* kInvalidMilestoneTransition =
    "ORCH_INVALID_MILESTONE_TRANSITION";
inline constexpr const char* kMilestoneNotFound = "ORCH_MILESTONE_NOT_FOUND";
inline constexpr const char* kMilestoneExists = "ORCH_MILESTONE_EXISTS";
}  // namespace error_codes

// Run-level states (design section 6.1). A run is in Executing while any
// milestone is active.
enum class RunState {
  Created,
  Ingesting,
  Refining,
  AwaitingDesignApproval,
  Executing,
  SystemValidation,
  Done,
  Escalated,
  Aborted,
};

// Events of the run-level state machine (design section 7.1).
enum class RunEvent {
  Start,
  IndexReady,
  DesignPackageReady,
  ChangesRequested,
  Approved,
  AllMilestonesDone,
  SystemDefectsFiled,
  SystemValidationPassed,
  BudgetExceeded,
  NoProgress,
  HumanResolved,
  HumanAborted,
  ApprovalDeadlineExpired,
  EscalationDeadlineExpired,
};

// Milestone-level states (design section 6.1). Each milestone has its own
// state machine.
enum class MilestoneState {
  Pending,  // dependencies not yet done
  Blocked,  // waiting for an integration or interface lock
  TestAuthoring,
  Coding,
  Gating,
  Reviewing,
  QaValidating,
  Fixing,
  Integrating,
  Revalidating,
  Done,
  Escalated,
};

// Events of the milestone state machine (design section 7.2).
enum class MilestoneEvent {
  Start,                         // dependencies done
  Block,                         // waiting for a lock (design 6.13.2)
  Unblock,                       // lock acquired / dependency merged
  TestsCommitted,                // QA committed the failing tests
  WorkSubmitted,                 // coder called submit_work
  GatesFailed,
  GatesPassed,
  GatesPassedReviewValid,        // gates pass and the review is still valid (7.4)
  ChangesRequested,              // reviewer verdict CHANGES_REQUESTED
  Approved,                      // reviewer verdict APPROVED
  DefectsFiled,                  // QA filed defects
  ExitCriteriaMet,               // all tests pass, exit criteria met
  StallOrBudget,                 // stall or budget limit (design section 12)
  Merged,                        // integrated, re-gated and still reviewed
  RebaseConflictOrRegateFailed,
  InterfaceChanged,              // a shared interface changed (6.13.3)
  GatesRerun,
  HumanResolved,                 // a person resolved the escalation
  WaiverGranted,                 // a human waiver covers the failing gate
};

std::string_view to_string(RunState state);
std::string_view to_string(RunEvent event);
std::string_view to_string(MilestoneState state);
std::string_view to_string(MilestoneEvent event);

std::optional<RunState> run_state_from_string(std::string_view text);
std::optional<MilestoneState> milestone_state_from_string(std::string_view text);

// True while the milestone occupies a scheduler slot (design sections 5.3 and
// 6.13.2). Pending, Blocked, Done and Escalated do not.
bool is_active(MilestoneState state);

class StateMachine {
 public:
  StateMachine(store::RunStore& store, std::string run_id);

  // Creates the milestone row in Pending. Fails if it is already registered.
  Status register_milestone(std::string_view milestone_id);

  // Applies a run-level event, persisting the transition and any operation
  // intents in one transaction (design section 6.12.2 step 1). Returns the
  // new state.
  Result<RunState> apply(RunEvent event,
                         const std::vector<store::NewOperation>& intents,
                         store::UnixTime now);

  // Applies a milestone-level event. WorkSubmitted increments the milestone's
  // iteration counter (design section 12: iterations per milestone).
  Result<MilestoneState> apply(std::string_view milestone_id, MilestoneEvent event,
                               store::UnixTime now);

  RunState current() const;
  Result<MilestoneState> current(std::string_view milestone_id) const;

  const std::string& run_id() const { return run_id_; }

 private:
  store::RunStore& store_;
  std::string run_id_;
};

}  // namespace cf::orch
