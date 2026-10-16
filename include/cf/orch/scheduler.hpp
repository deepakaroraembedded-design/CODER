// cf-orch: dependency-aware milestone scheduler (design section 6.13).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

struct SchedulerConfig {
  // scheduler.max_parallel_milestones (design section 5.3), default 2.
  uint32_t max_parallel_milestones = 2;
};

// What the scheduler asks the orchestrator to do. The orchestrator applies
// these as milestone events (Start / Block / Unblock) through the state
// machine, so every decision is persisted and auditable.
struct SchedulePlan {
  std::vector<std::string> start;    // Pending -> TestAuthoring (Start)
  std::vector<std::string> unblock;  // Blocked -> TestAuthoring (Unblock)
  std::vector<std::string> block;    // Pending -> Blocked (interface lock, 6.13.2)
};

class Scheduler {
 public:
  Scheduler(store::RunStore& store, std::string run_id, const plan::Plan& plan,
            SchedulerConfig config = {});

  // Pure decision, no state changes. Rules from design section 6.13.2:
  //   1. neither of two parallel milestones may depend on the other
  //      (a dependency must be Done before a milestone starts);
  //   2. their write sets must not overlap;
  //   3. neither may write an interface the other uses;
  // plus the parallelism cap. A pending milestone that conflicts on an
  // interface waits in Blocked; a path overlap or a full slot is a Wait
  // (no transition). Milestones are considered in plan order.
  Result<SchedulePlan> next() const;

 private:
  store::RunStore& store_;
  std::string run_id_;
  const plan::Plan& plan_;
  SchedulerConfig config_;
};

}  // namespace cf::orch
