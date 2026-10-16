#include "cf/orch/orchestrator.hpp"

#include <utility>

#include "cf/orch/escalation.hpp"

namespace cf::orch {

RunOrchestrator::RunOrchestrator(store::RunStore& store, std::string run_id,
                                 const plan::Plan& plan, MilestoneDriver& driver,
                                 OrchestratorConfig config)
    : store_(store),
      run_id_(std::move(run_id)),
      plan_(plan),
      driver_(driver),
      config_(std::move(config)),
      machine_(store, run_id_) {}

Result<std::optional<std::string>> RunOrchestrator::budget_exceeded() const {
  if (config_.max_usd_total.has_value()) {
    auto usd = store_.total_usd(run_id_);
    if (!usd) {
      return tl::make_unexpected(usd.error());
    }
    if (*usd >= *config_.max_usd_total) {
      return std::optional<std::string>(
          "run cost budget exceeded: $" + std::to_string(*usd) + " of $" +
          std::to_string(*config_.max_usd_total));
    }
  }
  if (config_.max_tokens_total.has_value()) {
    auto tokens = store_.total_tokens(run_id_);
    if (!tokens) {
      return tl::make_unexpected(tokens.error());
    }
    if (static_cast<uint64_t>(*tokens) >= *config_.max_tokens_total) {
      return std::optional<std::string>(
          "run token budget exceeded: " + std::to_string(*tokens) + " of " +
          std::to_string(*config_.max_tokens_total));
    }
  }
  return std::optional<std::string>{};
}

Result<bool> RunOrchestrator::all_milestones_done() const {
  for (const auto& spec : plan_.milestones) {
    const auto state = machine_.current(spec.id);
    if (!state) {
      return tl::make_unexpected(state.error());
    }
    if (*state != MilestoneState::Done) {
      return false;
    }
  }
  return true;
}

Result<RunState> RunOrchestrator::escalate(std::string_view reason, RunEvent event,
                                           store::UnixTime now) {
  last_escalation_reason_ = std::string(reason);
  const auto applied = machine_.apply(event, {}, now);
  if (!applied) {
    return tl::make_unexpected(applied.error());
  }
  if (!config_.escalation_dir.empty()) {
    auto bundle = write_escalation_bundle(store_, run_id_, config_.escalation_dir,
                                          reason, now);
    if (!bundle) {
      last_escalation_reason_ +=
          " (escalation bundle failed: " + bundle.error().message + ")";
    }
  }
  return *applied;
}

Status RunOrchestrator::ensure_milestones_registered() {
  for (const auto& spec : plan_.milestones) {
    const auto existing = store_.get_milestone(run_id_, spec.id);
    if (existing.has_value()) {
      continue;
    }
    if (existing.error().code != store::error_codes::kNotFound) {
      return tl::make_unexpected(existing.error());
    }
    const auto registered = machine_.register_milestone(spec.id);
    if (!registered) {
      return registered;
    }
  }
  return {};
}

Result<RunState> RunOrchestrator::step(store::UnixTime now) {
  const RunState state = machine_.current();

  if (state == RunState::SystemValidation) {
    const SystemValidationResult validation = driver_.validate_system();
    if (!validation.completed) {
      return state;
    }
    const auto applied = machine_.apply(
        validation.passed ? RunEvent::SystemValidationPassed
                          : RunEvent::SystemDefectsFiled,
        {}, now);
    if (!applied) {
      return tl::make_unexpected(applied.error());
    }
    return *applied;
  }

  if (state != RunState::Executing) {
    return state;
  }

  const auto registered = ensure_milestones_registered();
  if (!registered) {
    return tl::make_unexpected(registered.error());
  }

  Scheduler scheduler(store_, run_id_, plan_, config_.scheduler);
  MilestoneLoop loop(store_, machine_, driver_, config_.loop_limits);

  bool progressed = true;
  while (progressed) {
    progressed = false;

    auto exceeded = budget_exceeded();
    if (!exceeded) {
      return tl::make_unexpected(exceeded.error());
    }
    if (exceeded->has_value()) {
      return escalate(**exceeded, RunEvent::BudgetExceeded, now);
    }

    auto decision = scheduler.next();
    if (!decision) {
      return tl::make_unexpected(decision.error());
    }
    for (const auto& milestone_id : decision->start) {
      const auto applied = machine_.apply(milestone_id, MilestoneEvent::Start, now);
      if (!applied) {
        return tl::make_unexpected(applied.error());
      }
      progressed = true;
    }
    for (const auto& milestone_id : decision->unblock) {
      const auto applied = machine_.apply(milestone_id, MilestoneEvent::Unblock, now);
      if (!applied) {
        return tl::make_unexpected(applied.error());
      }
      progressed = true;
    }
    for (const auto& milestone_id : decision->block) {
      const auto applied = machine_.apply(milestone_id, MilestoneEvent::Block, now);
      if (!applied) {
        return tl::make_unexpected(applied.error());
      }
      progressed = true;
    }

    auto done = all_milestones_done();
    if (!done) {
      return tl::make_unexpected(done.error());
    }
    if (*done) {
      const auto applied = machine_.apply(RunEvent::AllMilestonesDone, {}, now);
      if (!applied) {
        return tl::make_unexpected(applied.error());
      }
      return *applied;
    }

    bool any_active = false;
    for (const auto& spec : plan_.milestones) {
      const auto current = machine_.current(spec.id);
      if (!current) {
        return tl::make_unexpected(current.error());
      }
      if (!is_active(*current)) {
        continue;
      }
      any_active = true;
      const MilestoneState before = *current;
      auto after = loop.drive(spec.id, now);
      if (!after) {
        return tl::make_unexpected(after.error());
      }
      if (*after != before) {
        progressed = true;
      }

      exceeded = budget_exceeded();
      if (!exceeded) {
        return tl::make_unexpected(exceeded.error());
      }
      if (exceeded->has_value()) {
        return escalate(**exceeded, RunEvent::BudgetExceeded, now);
      }
    }

    if (!any_active) {
      // Prefer the milestone's own escalation reason (turn failures, gate
      // loops, defect limits) over the generic run-level message.
      if (!loop.last_escalation_reason().empty()) {
        return escalate(loop.last_escalation_reason(), RunEvent::NoProgress, now);
      }
      return escalate("no milestone can progress", RunEvent::NoProgress, now);
    }
  }
  return machine_.current();
}

}  // namespace cf::orch
