// cf-orch: deadlines for states that wait for people (design section 12.1).
//
// AwaitingDesignApproval and Escalated do not spend tokens, but neither may
// wait forever. The deadline is a number of business days from the most
// recent transition into the waiting state; weekends are skipped. On expiry
// the run moves to Aborted with the design-package and escalation-timeout
// reasons from the design.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cf/common/result.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct ApprovalDeadlines {
  std::uint32_t design_approval_business_days = 5;
  std::uint32_t escalation_business_days = 3;
  // A pending waiver request is refused after this many business days
  // (design section 12.1). Recorded on the run so every command sees the
  // same value.
  std::uint32_t waiver_business_days = 2;
};

// Adds whole business days (Monday-Friday), preserving the time of day.
store::UnixTime add_business_days(store::UnixTime start, std::uint32_t days);

// Whole business days elapsed between two instants (0 when now <= start).
std::uint32_t business_days_between(store::UnixTime start, store::UnixTime now);

// The deadline of the run's current waiting state, or nullopt when the run is
// not waiting for a person.
Result<std::optional<store::UnixTime>> waiting_deadline(
    store::RunStore& store, std::string_view run_id,
    const ApprovalDeadlines& deadlines);

// Applies ApprovalDeadlineExpired / EscalationDeadlineExpired when the
// current waiting state is at or past its deadline. Returns the abort reason
// ("approval_timeout" or "escalation_timeout") when it expired.
Result<std::optional<std::string>> expire_waiting_run(
    store::RunStore& store, std::string_view run_id,
    const ApprovalDeadlines& deadlines, store::UnixTime now);

}  // namespace cf::orch
