#include "cf/orch/scheduler.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace cf::orch {
namespace {

bool intersects(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  for (const auto& left : a) {
    if (std::find(b.begin(), b.end(), left) != b.end()) {
      return true;
    }
  }
  return false;
}

}  // namespace

Scheduler::Scheduler(store::RunStore& store, std::string run_id, const plan::Plan& plan,
                     SchedulerConfig config)
    : store_(store), run_id_(std::move(run_id)), plan_(plan), config_(config) {}

Result<SchedulePlan> Scheduler::next() const {
  // Current state of every plan milestone; a milestone that has not been
  // registered yet is treated as Pending.
  std::map<std::string, MilestoneState, std::less<>> states;
  for (const auto& milestone : plan_.milestones) {
    const auto stored = store_.get_milestone(run_id_, milestone.id);
    if (stored.has_value()) {
      const auto state = milestone_state_from_string(stored->state);
      if (!state) {
        return fail(error_codes::kInvalidMilestoneTransition,
                    "milestone " + milestone.id + " has unknown stored state '" +
                        stored->state + "'");
      }
      states.emplace(milestone.id, *state);
    } else if (stored.error().code == store::error_codes::kNotFound) {
      states.emplace(milestone.id, MilestoneState::Pending);
    } else {
      return tl::make_unexpected(stored.error());
    }
  }

  // Occupied scheduler slots: active milestones plus the ones selected in
  // this pass.
  std::vector<const plan::MilestoneSpec*> occupants;
  for (const auto& milestone : plan_.milestones) {
    if (is_active(states.at(milestone.id))) {
      occupants.push_back(&milestone);
    }
  }

  const auto dependencies_done = [&](const plan::MilestoneSpec& spec) {
    for (const auto& dependency : spec.depends_on) {
      const auto state = states.find(dependency);
      if (state == states.end() || state->second != MilestoneState::Done) {
        return false;
      }
    }
    return true;
  };

  const auto interface_conflict = [&](const plan::MilestoneSpec& spec) {
    for (const auto* occupant : occupants) {
      if (intersects(spec.uses, occupant->writes_interfaces) ||
          intersects(occupant->uses, spec.writes_interfaces)) {
        return true;
      }
    }
    return false;
  };

  const auto write_overlap = [&](const plan::MilestoneSpec& spec) {
    for (const auto* occupant : occupants) {
      if (intersects(spec.writes, occupant->writes)) {
        return true;
      }
    }
    return false;
  };

  const auto capacity_full = [&]() {
    return occupants.size() >= config_.max_parallel_milestones;
  };

  SchedulePlan plan;
  for (const auto& milestone : plan_.milestones) {
    const MilestoneState state = states.at(milestone.id);
    if (state == MilestoneState::Pending) {
      if (!dependencies_done(milestone)) {
        continue;  // dependency not Done yet: Wait
      }
      if (interface_conflict(milestone)) {
        plan.block.push_back(milestone.id);
        continue;
      }
      if (write_overlap(milestone) || capacity_full()) {
        continue;  // Wait without a transition
      }
      plan.start.push_back(milestone.id);
      occupants.push_back(&milestone);
    } else if (state == MilestoneState::Blocked) {
      if (!dependencies_done(milestone) || interface_conflict(milestone) ||
          write_overlap(milestone) || capacity_full()) {
        continue;  // stay Blocked
      }
      plan.unblock.push_back(milestone.id);
      occupants.push_back(&milestone);
    }
  }
  return plan;
}

}  // namespace cf::orch
