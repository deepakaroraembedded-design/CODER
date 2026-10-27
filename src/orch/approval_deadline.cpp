#include "cf/orch/approval_deadline.hpp"

#include <utility>

#include "cf/orch/state_machine.hpp"

namespace cf::orch {
namespace {

constexpr store::UnixTime kSecondsPerDay = 24 * 60 * 60;

// Day 0 (1970-01-01) was a Thursday. With 0 = Sunday, weekday is
// (day + 4) % 7.
int weekday(store::UnixTime at) {
  const store::UnixTime day = at / kSecondsPerDay;
  return static_cast<int>((day + 4) % 7);
}

bool is_weekend(store::UnixTime at) {
  const int day = weekday(at);
  return day == 0 || day == 6;
}

Result<store::UnixTime> waiting_since(store::RunStore& store,
                                      std::string_view run_id,
                                      std::string_view waiting_state,
                                      const store::RunRecord& run) {
  auto transitions = store.list_transitions(run_id);
  if (!transitions) {
    return tl::make_unexpected(transitions.error());
  }
  store::UnixTime at = 0;
  for (const auto& transition : *transitions) {
    if (transition.to_state == waiting_state) {
      at = transition.at;
    }
  }
  // A run created directly in the waiting state has no transition yet.
  return at == 0 ? run.created_at : at;
}

}  // namespace

store::UnixTime add_business_days(store::UnixTime start,
                                  std::uint32_t days) {
  store::UnixTime at = start;
  std::uint32_t added = 0;
  while (added < days) {
    at += kSecondsPerDay;
    if (!is_weekend(at)) {
      ++added;
    }
  }
  return at;
}

Result<std::optional<store::UnixTime>> waiting_deadline(
    store::RunStore& store, std::string_view run_id,
    const ApprovalDeadlines& deadlines) {
  auto run = store.get_run(run_id);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  const auto state = run_state_from_string(run->state);
  if (!state) {
    return std::optional<store::UnixTime>{};
  }
  std::uint32_t days = 0;
  switch (*state) {
    case RunState::AwaitingDesignApproval:
      days = deadlines.design_approval_business_days;
      break;
    case RunState::Escalated:
      days = deadlines.escalation_business_days;
      break;
    case RunState::Created:
    case RunState::Ingesting:
    case RunState::Refining:
    case RunState::Executing:
    case RunState::SystemValidation:
    case RunState::Done:
    case RunState::Aborted:
      return std::optional<store::UnixTime>{};
  }
  auto since = waiting_since(store, run_id, run->state, *run);
  if (!since) {
    return tl::make_unexpected(since.error());
  }
  return std::optional<store::UnixTime>(add_business_days(*since, days));
}

Result<std::optional<std::string>> expire_waiting_run(
    store::RunStore& store, std::string_view run_id,
    const ApprovalDeadlines& deadlines, store::UnixTime now) {
  auto run = store.get_run(run_id);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  RunEvent event = RunEvent::ApprovalDeadlineExpired;
  std::string reason;
  if (run->state == to_string(RunState::AwaitingDesignApproval)) {
    event = RunEvent::ApprovalDeadlineExpired;
    reason = "approval_timeout";
  } else if (run->state == to_string(RunState::Escalated)) {
    event = RunEvent::EscalationDeadlineExpired;
    reason = "escalation_timeout";
  } else {
    return std::optional<std::string>{};
  }

  auto deadline = waiting_deadline(store, run_id, deadlines);
  if (!deadline) {
    return tl::make_unexpected(deadline.error());
  }
  if (!deadline->has_value() || now < **deadline) {
    return std::optional<std::string>{};
  }

  StateMachine machine(store, std::string(run_id));
  auto applied = machine.apply(event, {}, now);
  if (!applied) {
    return tl::make_unexpected(applied.error());
  }
  return std::optional<std::string>(std::move(reason));
}

}  // namespace cf::orch
