#include "cf/orch/milestone_loop.hpp"

#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cf/common/hash.hpp"

namespace cf::orch {
namespace {

// A stable fingerprint of the failing findings, so the same gate failure can
// be recognised across runs even though the full report changes (durations).
std::string failure_fingerprint(const cf::gates::GateReport& report) {
  std::string canonical;
  for (const auto& result : report.results) {
    if (result.passed) {
      continue;
    }
    canonical += result.gate;
    canonical.push_back('\n');
    for (const auto& finding : result.findings) {
      canonical += finding.severity;
      canonical.push_back(':');
      canonical += finding.message;
      canonical.push_back('\n');
    }
  }
  return cf::sha256_hex(canonical);
}

// How many times the coder has disputed a test, from the dispute defect's
// body (written by the cf-mcp dispute_test tool).
int dispute_count(const std::string& body_json) {
  const nlohmann::json body = nlohmann::json::parse(body_json, nullptr, false);
  if (!body.is_object()) {
    return 1;
  }
  return body.value("disputes", 1);
}

}  // namespace

MilestoneLoop::MilestoneLoop(store::RunStore& store, StateMachine& machine,
                             MilestoneDriver& driver, LoopLimits limits)
    : store_(store), machine_(machine), driver_(driver), limits_(limits) {}

Result<MilestoneState> MilestoneLoop::escalate(std::string_view milestone_id,
                                               std::string reason, store::UnixTime now) {
  last_escalation_reason_ = std::move(reason);
  spdlog::warn("milestone {} escalated: {}", milestone_id, last_escalation_reason_);
  const auto applied =
      machine_.apply(milestone_id, MilestoneEvent::StallOrBudget, now);
  if (!applied) {
    return tl::make_unexpected(applied.error());
  }
  return *applied;
}

Result<MilestoneState> MilestoneLoop::drive(std::string_view milestone_id,
                                            store::UnixTime now) {
  const std::string id(milestone_id);
  uint32_t consecutive_turn_failures = 0;
  uint32_t consecutive_same_gate_failures = 0;
  std::string last_gate_fingerprint;
  uint32_t no_progress_iterations = 0;
  int64_t last_passing_tests = -1;
  last_escalation_reason_.clear();

  for (;;) {
    const auto state = machine_.current(milestone_id);
    if (!state) {
      return tl::make_unexpected(state.error());
    }

    switch (*state) {
      case MilestoneState::Pending:
      case MilestoneState::Blocked:
      case MilestoneState::Done:
      case MilestoneState::Escalated:
        return *state;

      case MilestoneState::TestAuthoring: {
        const TurnResult result = driver_.run(MilestonePhase::AuthorTests, id);
        if (!result.completed) {
          ++consecutive_turn_failures;
          if (consecutive_turn_failures >= limits_.max_consecutive_turn_failures) {
            return escalate(milestone_id,
                            "QA test authoring failed " +
                                std::to_string(consecutive_turn_failures) +
                                " times in a row: " + result.detail,
                            now);
          }
          continue;
        }
        consecutive_turn_failures = 0;
        const auto applied =
            machine_.apply(milestone_id, MilestoneEvent::TestsCommitted, now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        break;
      }

      case MilestoneState::Coding:
      case MilestoneState::Fixing: {
        const auto milestone = store_.get_milestone(machine_.run_id(), id);
        if (!milestone) {
          return tl::make_unexpected(milestone.error());
        }
        if (milestone->iterations >= limits_.max_iterations_per_milestone) {
          return escalate(milestone_id,
                          "iteration limit reached (" +
                              std::to_string(milestone->iterations) + " of " +
                              std::to_string(limits_.max_iterations_per_milestone) + ")",
                          now);
        }
        // Every fix turn counts as one attempt for each open defect of the
        // milestone; a defect past its limit escalates (design section 12).
        if (*state == MilestoneState::Fixing) {
          auto defects = store_.list_defects(machine_.run_id(), id,
                                             store::open_defect_statuses());
          if (!defects) {
            return tl::make_unexpected(defects.error());
          }
          for (const auto& defect : *defects) {
            // A disputed test is not a code defect: it is counted by the
            // dispute rule below, not by fix attempts (design section 12).
            if (defect.status == "DISPUTED") {
              continue;
            }
            if (defect.attempts >= limits_.max_attempts_per_defect) {
              return escalate(milestone_id,
                              "defect " + defect.id + " reached " +
                                  std::to_string(defect.attempts) + " of " +
                                  std::to_string(limits_.max_attempts_per_defect) +
                                  " fix attempts",
                              now);
            }
            const auto stored = store_.set_defect_status(defect.id, defect.status,
                                                         defect.attempts + 1);
            if (!stored) {
              return tl::make_unexpected(stored.error());
            }
          }
        }
        const MilestonePhase phase =
            *state == MilestoneState::Coding ? MilestonePhase::Code : MilestonePhase::Fix;
        const TurnResult result = driver_.run(phase, id);
        if (!result.completed) {
          ++consecutive_turn_failures;
          if (consecutive_turn_failures >= limits_.max_consecutive_turn_failures) {
            return escalate(milestone_id,
                            "coder turn failed " +
                                std::to_string(consecutive_turn_failures) +
                                " times in a row: " + result.detail,
                            now);
          }
          continue;
        }
        if (!result.disputed_test_id.empty()) {
          // The coder proposed a change to a protected test: QA decides
          // immediately (design section 12). Disputing the same test again
          // after a rejection escalates instead of looping.
          const std::string fingerprint = "test-dispute:" + result.disputed_test_id;
          const auto dispute = store_.find_defect_by_fingerprint(machine_.run_id(),
                                                                 fingerprint);
          if (dispute.has_value() && dispute_count(dispute->body_json) >= 2) {
            return escalate(milestone_id,
                            "the coder disputed test " + result.disputed_test_id +
                                " again after QA rejected it",
                            now);
          }
          const TurnResult decision = driver_.run_test_dispute(id);
          if (!decision.completed) {
            ++consecutive_turn_failures;
            if (consecutive_turn_failures >= limits_.max_consecutive_turn_failures) {
              return escalate(milestone_id,
                              "the test dispute decision failed " +
                                  std::to_string(consecutive_turn_failures) +
                                  " times in a row: " + decision.detail,
                              now);
            }
            continue;
          }
          // The decision counts as the coder's submission: the work that came
          // with the dispute goes to the gates with the corrected test.
        }
        consecutive_turn_failures = 0;
        const auto applied =
            machine_.apply(milestone_id, MilestoneEvent::WorkSubmitted, now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        break;
      }

      case MilestoneState::Gating: {
        const GateOutcome outcome = driver_.run_gates(id);

        if (!outcome.passed) {
          const std::string fingerprint = failure_fingerprint(outcome.report);
          if (fingerprint == last_gate_fingerprint) {
            ++consecutive_same_gate_failures;
          } else {
            last_gate_fingerprint = fingerprint;
            consecutive_same_gate_failures = 1;
          }
          if (consecutive_same_gate_failures >= limits_.max_consecutive_gate_failures) {
            return escalate(milestone_id,
                            "the same gate failure repeated " +
                                std::to_string(consecutive_same_gate_failures) +
                                " times in a row",
                            now);
          }
          const auto applied =
              machine_.apply(milestone_id, MilestoneEvent::GatesFailed, now);
          if (!applied) {
            return tl::make_unexpected(applied.error());
          }
        } else {
          consecutive_same_gate_failures = 0;
          last_gate_fingerprint.clear();
          const ReviewKind review =
              decide_review(outcome.changes_since_approved, outcome.review_context);
          const auto applied = machine_.apply(
              milestone_id,
              review == ReviewKind::None ? MilestoneEvent::GatesPassedReviewValid
                                         : MilestoneEvent::GatesPassed,
              now);
          if (!applied) {
            return tl::make_unexpected(applied.error());
          }
        }

        if (outcome.passing_tests >= 0) {
          if (last_passing_tests >= 0 && outcome.passing_tests <= last_passing_tests) {
            ++no_progress_iterations;
            if (no_progress_iterations >= limits_.max_no_progress_iterations) {
              return escalate(milestone_id,
                              "no net progress: passing tests did not increase for " +
                                  std::to_string(no_progress_iterations) +
                                  " iterations",
                              now);
            }
          } else {
            no_progress_iterations = 0;
          }
          last_passing_tests = outcome.passing_tests;
        }
        break;
      }

      case MilestoneState::Reviewing: {
        const TurnResult result = driver_.run(MilestonePhase::Review, id);
        if (!result.completed) {
          ++consecutive_turn_failures;
          if (consecutive_turn_failures >= limits_.max_consecutive_turn_failures) {
            return escalate(milestone_id,
                            "reviewer turn failed " +
                                std::to_string(consecutive_turn_failures) +
                                " times in a row: " + result.detail,
                            now);
          }
          continue;
        }
        consecutive_turn_failures = 0;
        const auto applied = machine_.apply(
            milestone_id,
            result.changes_requested ? MilestoneEvent::ChangesRequested
                                     : MilestoneEvent::Approved,
            now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        break;
      }

      case MilestoneState::QaValidating: {
        const TurnResult result = driver_.run(MilestonePhase::Validate, id);
        if (!result.completed) {
          ++consecutive_turn_failures;
          if (consecutive_turn_failures >= limits_.max_consecutive_turn_failures) {
            return escalate(milestone_id,
                            "QA validation failed " +
                                std::to_string(consecutive_turn_failures) +
                                " times in a row: " + result.detail,
                            now);
          }
          continue;
        }
        consecutive_turn_failures = 0;
        // Exit criteria are objective (design section 11.1): a milestone is
        // not done while open defects remain, whatever the agent submitted.
        // QA has to verify or close them (for example a stale defect).
        bool exit_met = result.exit_criteria_met;
        if (exit_met) {
          auto open = store_.list_defects(machine_.run_id(), id,
                                          store::open_defect_statuses());
          if (!open) {
            return tl::make_unexpected(open.error());
          }
          exit_met = open->empty();
        }
        const auto applied = machine_.apply(
            milestone_id,
            exit_met ? MilestoneEvent::ExitCriteriaMet : MilestoneEvent::DefectsFiled,
            now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        break;
      }

      case MilestoneState::Integrating: {
        const TurnResult result = driver_.run(MilestonePhase::Integrate, id);
        if (!result.completed) {
          ++consecutive_turn_failures;
          if (consecutive_turn_failures >= limits_.max_consecutive_turn_failures) {
            return escalate(milestone_id,
                            "integration failed " +
                                std::to_string(consecutive_turn_failures) +
                                " times in a row: " + result.detail,
                            now);
          }
          continue;
        }
        consecutive_turn_failures = 0;
        const auto applied = machine_.apply(
            milestone_id,
            result.merged ? MilestoneEvent::Merged
                          : MilestoneEvent::RebaseConflictOrRegateFailed,
            now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        break;
      }

      case MilestoneState::Revalidating: {
        const auto applied =
            machine_.apply(milestone_id, MilestoneEvent::GatesRerun, now);
        if (!applied) {
          return tl::make_unexpected(applied.error());
        }
        break;
      }
    }
  }
}

}  // namespace cf::orch
