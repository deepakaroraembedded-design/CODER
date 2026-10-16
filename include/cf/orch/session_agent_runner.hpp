// cf-orch: agent runner that resolves a session driver per role (design
// section 6.6). One OpenCodeAgentRunner is kept per (role, worktree) so the
// MCP calls-file offset survives across turns; the driver itself comes from a
// factory (the production factory spawns opencode, tests inject fakes).
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "cf/orch/opencode_runner.hpp"

namespace cf::orch {

using SessionDriverFactory =
    std::function<Result<std::unique_ptr<SessionDriver>>(const std::string& role,
                                                         const std::string& worktree)>;

class SessionAgentRunner final : public AgentRunner {
 public:
  SessionAgentRunner(SessionDriverFactory factory, std::filesystem::path calls_dir,
                     OpencodeRunnerOptions base_options);

  Result<AgentTurnOutcome> run_turn(const AgentTurnRequest& request) override;
  Result<AgentTurnOutcome> run_system_validation(
      const AgentTurnRequest& request) override;

 private:
  OpencodeAgentRunner* runner_for(const AgentTurnRequest& request);

  SessionDriverFactory factory_;
  std::filesystem::path calls_dir_;
  OpencodeRunnerOptions base_;
  std::map<std::string, std::unique_ptr<SessionDriver>, std::less<>> drivers_;
  std::map<std::string, std::unique_ptr<OpencodeAgentRunner>, std::less<>> runners_;
};

}  // namespace cf::orch
