#include "cf/orch/session_agent_runner.hpp"

#include <utility>

#include <spdlog/spdlog.h>

namespace cf::orch {

SessionAgentRunner::SessionAgentRunner(SessionDriverFactory factory,
                                       std::filesystem::path calls_dir,
                                       OpencodeRunnerOptions base_options)
    : factory_(std::move(factory)),
      calls_dir_(std::move(calls_dir)),
      base_(std::move(base_options)) {}

OpencodeAgentRunner* SessionAgentRunner::runner_for(const AgentTurnRequest& request) {
  const std::string key = request.role + "|" + request.worktree;
  auto existing = runners_.find(key);
  if (existing != runners_.end()) {
    if (existing->second->healthy()) {
      return existing->second.get();
    }
    // The opencode server died: drop the cached runner and driver so the
    // factory starts a fresh server for the same role and worktree before
    // the next turn (design section 6.12, scenario H8).
    spdlog::warn("opencode server for {} died; recreating it", key);
    runners_.erase(existing);
    drivers_.erase(key);
  }
  auto driver = factory_(request.role, request.worktree);
  if (!driver) {
    return nullptr;
  }
  OpencodeRunnerOptions options = base_;
  options.calls_file =
      (calls_dir_ / ("calls-" + request.role + ".jsonl")).string();
  options.usage_file = calls_dir_ / ("usage-" + request.role + ".jsonl");
  auto runner = std::make_unique<OpencodeAgentRunner>(**driver, options);
  drivers_.emplace(key, std::move(*driver));
  auto* raw = runner.get();
  runners_.emplace(key, std::move(runner));
  return raw;
}

Result<AgentTurnOutcome> SessionAgentRunner::run_turn(const AgentTurnRequest& request) {
  OpencodeAgentRunner* runner = runner_for(request);
  if (runner == nullptr) {
    return fail("ORCH_DRIVER_UNAVAILABLE",
                "cannot create a session driver for role " + request.role);
  }
  return runner->run_turn(request);
}

Result<AgentTurnOutcome> SessionAgentRunner::run_system_validation(
    const AgentTurnRequest& request) {
  AgentTurnRequest validation = request;
  if (validation.role.empty()) {
    validation.role = "qa";
  }
  if (validation.worktree.empty()) {
    // System validation has no milestone worktree; use the integration one.
    return fail("ORCH_DRIVER_UNAVAILABLE",
                "system validation needs a worktree in the request");
  }
  OpencodeAgentRunner* runner = runner_for(validation);
  if (runner == nullptr) {
    return fail("ORCH_DRIVER_UNAVAILABLE",
                "cannot create a session driver for role " + validation.role);
  }
  return runner->run_system_validation(validation);
}

}  // namespace cf::orch
