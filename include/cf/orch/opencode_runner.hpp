// cf-orch: the opencode-backed agent runner (design sections 6.6, 6.6.1).
//
// Drives one agent turn: create a session, send the role prompt, wait for the
// turn to go idle, and read the structured submission from the role's MCP
// calls file. A turn that ends without a submission gets the fixed
// continuation message, at most `max_continuations` times (design section
// 6.6.1 layer 3); a watchdog timeout aborts the session.
//
// The SessionDriver slice is implemented with the cf-agents adapter in
// opencode_session_driver.hpp and faked in tests.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
#include "cf/orch/agent_runner.hpp"
#include "cf/orch/prompt_library.hpp"

namespace cf::orch {

namespace error_codes {
inline constexpr const char* kInvalidSession = "ORCH_INVALID_SESSION";
inline constexpr const char* kTurnTimeout = "ORCH_TURN_TIMEOUT";
inline constexpr const char* kNoSubmission = "ORCH_NO_SUBMISSION";
inline constexpr const char* kUnexpectedSubmission = "ORCH_UNEXPECTED_SUBMISSION";
}  // namespace error_codes

class SessionDriver {
 public:
  virtual ~SessionDriver() = default;

  virtual Result<std::string> create_session() = 0;
  virtual Status send_prompt(std::string_view session_id, std::string_view text) = 0;
  // Blocks until the session is idle; a timeout returns an error and the
  // runner aborts the session. A positive `stall_timeout` fails the wait
  // early when no event for the session arrives for that long (design
  // section 6.6.1 layer 3: no events for 120 s aborts the turn).
  virtual Status wait_idle(std::string_view session_id,
                           std::chrono::milliseconds timeout,
                           std::chrono::milliseconds stall_timeout) = 0;
  virtual Status abort(std::string_view session_id) = 0;
  // Best-effort usage for a finished session (provider cost and tokens);
  // drivers that cannot report it return an error (design section 6.11).
  virtual Result<nlohmann::json> session_usage(std::string_view session_id);
  // False when the backing opencode server died; the runner then recreates
  // the server before the next turn (design section 6.12, H8).
  virtual bool healthy() const { return true; }
};

struct OpencodeRunnerOptions {
  // The role's CF_MCP_CALLS_FILE (written by the role's cf-mcp instance).
  std::string calls_file;
  // When set, one JSON line per finished turn with the session usage
  // (provider cost and tokens); used by the cost ledger and the model
  // benchmark. Empty disables it.
  std::filesystem::path usage_file;
  std::chrono::milliseconds turn_timeout{600000};
  // No-events watchdog inside a turn: a session that emits nothing for this
  // long is stalled (a hung provider stream emits no events at all), so the
  // turn is aborted instead of waiting out `turn_timeout` (design section
  // 6.6.1 layer 3). Zero disables it.
  std::chrono::milliseconds stall_timeout{120000};
  // Fixed continuation attempts before the turn counts as failed.
  uint32_t max_continuations = 2;
  std::string target_language = "cpp";
  std::string target_standard = "c++20";
  // The profile's agent commands (build/test), injected into the prompt so
  // the model does not waste turns on denied shell commands. The read-only
  // git commands are appended per role (design section 6.6.1 layer 1).
  std::string allowed_commands;
  std::string continuation_message =
      "Proceed with the task as specified. Do not ask questions. Finish by "
      "calling the submission tool.";
  // Versioned role templates (prompts/<role>.md); embedded defaults apply
  // when the directory is not provided.
  PromptLibrary prompts = PromptLibrary::defaults();
};

class OpencodeAgentRunner final : public AgentRunner {
 public:
  OpencodeAgentRunner(SessionDriver& driver, OpencodeRunnerOptions options);

  Result<AgentTurnOutcome> run_turn(const AgentTurnRequest& request) override;
  Result<AgentTurnOutcome> run_system_validation(
      const AgentTurnRequest& request) override;

  // The prompt built for a turn (exposed for tests and prompt review).
  std::string build_prompt(const AgentTurnRequest& request,
                           bool system_validation) const;

  // False when the driver's opencode server died.
  bool healthy() const { return driver_.healthy(); }

 private:
  Result<AgentTurnOutcome> drive(const AgentTurnRequest& request,
                                 bool system_validation);
  Result<std::vector<nlohmann::json>> read_new_calls();
  Result<AgentTurnOutcome> outcome_from_submission(const AgentTurnRequest& request,
                                                   const nlohmann::json& call) const;
  void record_usage(const AgentTurnRequest& request, std::string_view session_id);

  SessionDriver& driver_;
  OpencodeRunnerOptions options_;
  std::size_t calls_offset_ = 0;
  bool calls_offset_initialized_ = false;
};

}  // namespace cf::orch
