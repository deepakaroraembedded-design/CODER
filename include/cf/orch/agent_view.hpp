// cf-orch: the agent's read-only view of the run store (design section 6.8.1).
//
// Agents never write the run store. A sandboxed (or host) cf-mcp reads a
// disposable snapshot written by the orchestrator before each turn; the
// agent's own writes are recorded as intents in its calls file and applied
// to the real store by the orchestrator (agent_intents.hpp).
#pragma once

#include <filesystem>
#include <string>

#include "cf/common/hash.hpp"

namespace cf::orch {

inline std::filesystem::path agent_dir(const std::filesystem::path& run_dir) {
  return run_dir / "agent";
}

// One snapshot and one calls file per role and worktree, so parallel
// milestones never share a view.
inline std::string agent_view_key(const std::string& role,
                                  const std::string& worktree) {
  return cf::sha256_hex(role + "|" + worktree).substr(0, 16);
}

inline std::filesystem::path agent_view_path(const std::filesystem::path& run_dir,
                                             const std::string& role,
                                             const std::string& worktree) {
  return agent_dir(run_dir) / "views" / (agent_view_key(role, worktree) + ".db");
}

inline std::filesystem::path agent_calls_path(const std::filesystem::path& run_dir,
                                              const std::string& role) {
  return agent_dir(run_dir) / ("calls-" + role + ".jsonl");
}

inline std::filesystem::path agent_log_path(const std::filesystem::path& run_dir,
                                            const std::string& role) {
  return agent_dir(run_dir) / "logs" / ("opencode-" + role + ".log");
}

}  // namespace cf::orch
