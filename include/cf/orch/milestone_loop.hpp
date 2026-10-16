// cf-orch: the milestone loop controller (design sections 7.2, 7.3, 12).
//
// Drives one milestone through the core cycle: QA authors tests, the coder
// implements, gates run, the review-validity rules decide whether the
// reviewer runs, QA validates, and the milestone is integrated. Loop control
// (iteration, stall, no-progress and consecutive-failure limits) escalates
// instead of looping forever.
//
// The MilestoneDriver is the integration point: the real implementation wires
// the agents (cf-agents), git and the staged gate runner (cf-gates); tests
// inject a scripted driver.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/gates/runner.hpp"
#include "cf/orch/review_policy.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

enum class MilestonePhase { AuthorTests, Code, Fix, Review, Validate, Integrate };

// Result of one agent turn as seen by the loop.
struct TurnResult {
  bool completed = false;  // the agent ended with its structured submission
  std::string summary;
  std::string detail;  // failure detail when not completed

  bool changes_requested = false;  // Review: verdict CHANGES_REQUESTED
  bool exit_criteria_met = false;  // Validate: all tests pass, exit criteria met
  bool merged = false;             // Integrate: merge succeeded
  bool conflicts = false;          // Integrate: rebase conflict (-> Fixing)
  // Integrate: a public interface changed; the orchestrator moves every
  // milestone that uses it (including Done ones) to Revalidating (6.13.3).
  bool interface_changed = false;
};

struct GateOutcome {
  bool passed = false;
  std::string commit_sha;
  cf::gates::GateReport report;
  // Review decision inputs (design section 7.4).
  std::vector<FileChange> changes_since_approved;
  ReviewContext review_context;
  // Passing test count for the no-progress check (design section 12);
  // -1 means unknown and disables the check for this run.
  int64_t passing_tests = -1;
};

// Result of the run-level system validation (design section 11.2).
struct SystemValidationResult {
  bool completed = false;
  bool passed = false;
  std::string detail;
};

class MilestoneDriver {
 public:
  virtual ~MilestoneDriver() = default;

  virtual TurnResult run(MilestonePhase phase, std::string_view milestone_id) = 0;
  virtual GateOutcome run_gates(std::string_view milestone_id) = 0;

  // Run-level hook: runs the full system validation suite on the merged
  // result (design section 11.2). The default reports "not completed".
  virtual SystemValidationResult validate_system() { return {}; }
};

struct LoopLimits {
  uint32_t max_iterations_per_milestone = 12;  // design section 12
  uint32_t max_consecutive_gate_failures = 3;  // same failure fingerprint
  uint32_t max_consecutive_turn_failures = 3;
  uint32_t max_no_progress_iterations = 4;     // passing tests not increasing
  // Attempts per defect before that defect escalates (design section 12).
  uint32_t max_attempts_per_defect = 4;
};

class MilestoneLoop {
 public:
  MilestoneLoop(store::RunStore& store, StateMachine& machine, MilestoneDriver& driver,
                LoopLimits limits = {});

  // Drives the milestone until it is Done, Escalated, or waiting on an
  // external event (Pending or Blocked). Returns the resulting state; when
  // the state is Escalated, last_escalation_reason() explains why.
  Result<MilestoneState> drive(std::string_view milestone_id, store::UnixTime now);

  const std::string& last_escalation_reason() const { return last_escalation_reason_; }

 private:
  Result<MilestoneState> escalate(std::string_view milestone_id, std::string reason,
                                  store::UnixTime now);

  store::RunStore& store_;
  StateMachine& machine_;
  MilestoneDriver& driver_;
  LoopLimits limits_;
  std::string last_escalation_reason_;
};

}  // namespace cf::orch
