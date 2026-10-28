#include "cf/orch/opencode_runner.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace cf::orch {
namespace {

using Json = nlohmann::json;

// The tool call that ends a turn, per phase (design section 9.1).
bool accepts_submission(MilestonePhase phase, const std::string& tool) {
  switch (phase) {
    case MilestonePhase::AuthorTests:
      return tool == "submit_tests";
    case MilestonePhase::Code:
    case MilestonePhase::Fix:
      // dispute_test ends the turn too: a protected test that cannot pass is
      // decided by QA before the coder keeps going (design sections 6.9, 12).
      return tool == "submit_work" || tool == "dispute_test";
    case MilestonePhase::Review:
      return tool == "submit_review";
    case MilestonePhase::Validate:
      return tool == "submit_tests" || tool == "file_defect" || tool == "verify_defect";
    case MilestonePhase::Integrate:
      return false;  // integration is git work, not an agent turn
  }
  return false;
}


std::string join(const std::vector<std::string>& values) {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) {
      out += ", ";
    }
    out += value;
  }
  return out;
}

// The shell commands the role's static config actually allows (design
// section 6.6.1 layer 1), so the prompt matches the policy.
std::string commands_for(const OpencodeRunnerOptions& options,
                         const std::string& role) {
  if (role == "reviewer") {
    return "git status, git diff, git log, grep, cat, head";
  }
  std::string commands = options.allowed_commands;
  if (role == "coder") {
    if (!commands.empty()) {
      commands += ", ";
    }
    commands += "git status, git diff, git log";
  }
  if (commands.empty()) {
    return "the toolchain's build and test commands";
  }
  return commands;
}

std::string submission_tools_for(MilestonePhase phase) {
  switch (phase) {
    case MilestonePhase::AuthorTests:
      return "submit_tests";
    case MilestonePhase::Code:
    case MilestonePhase::Fix:
      return "submit_work, or dispute_test when a protected test under tests/ "
             "cannot compile or contradicts the interface";
    case MilestonePhase::Review:
      return "submit_review";
    case MilestonePhase::Validate:
      return "submit_tests, file_defect or verify_defect";
    case MilestonePhase::Integrate:
      return "none (integration is orchestrator work)";
  }
  return "submit_work";
}

}  // namespace

Result<Json> SessionDriver::session_usage(std::string_view) {
  return fail("ORCH_USAGE_UNAVAILABLE", "this driver cannot report session usage");
}

OpencodeAgentRunner::OpencodeAgentRunner(SessionDriver& driver,
                                         OpencodeRunnerOptions options)
    : driver_(driver), options_(std::move(options)) {}

void OpencodeAgentRunner::record_usage(const AgentTurnRequest& request,
                                       std::string_view session_id) {
  if (options_.usage_file.empty()) {
    return;
  }
  auto usage = driver_.session_usage(session_id);
  if (!usage.has_value()) {
    return;  // best effort; the ledger simply lacks this turn
  }
  Json line;
  line["role"] = request.role;
  line["milestone_id"] = request.milestone_id;
  line["session_id"] = std::string(session_id);
  line["usage"] = *usage;
  std::ofstream out(options_.usage_file, std::ios::app | std::ios::binary);
  if (out) {
    out << line.dump() << "\n";
  }
}

std::string OpencodeAgentRunner::build_prompt(const AgentTurnRequest& request,
                                              bool system_validation) const {
  std::map<std::string, std::string, std::less<>> values;
  values["run_id"] = request.run_id;
  values["milestone_id"] = request.milestone_id;
  values["language"] = options_.target_language;
  values["standard"] = options_.target_standard;
  values["requirements"] = join(request.requirement_ids);
  values["open_defects"] = join(request.open_defect_ids);
  values["blocking_severities"] =
      options_.blocking_severities.empty() ? "none"
                                           : join(options_.blocking_severities);
  values["brief"] = system_validation
                        ? "Run the full system validation suite on the merged result"
                        : request.brief;
  values["submission_tools"] = submission_tools_for(request.phase);
  values["commands"] = commands_for(options_, request.role);
  auto rendered = options_.prompts.render(request.role, values);
  if (!rendered) {
    // Fall back to the embedded defaults rather than failing the turn.
    PromptLibrary defaults = PromptLibrary::defaults();
    rendered = defaults.render(request.role, values);
    if (!rendered) {
      return "Task: " + values["brief"] +
             "\n\nDo not ask questions. Finish by calling the submission tool (" +
             values["submission_tools"] + ").";
    }
  }
  return *rendered;
}

Result<std::vector<Json>> OpencodeAgentRunner::read_new_calls() {
  std::vector<Json> calls;
  if (options_.calls_file.empty()) {
    return calls;
  }
  std::ifstream in(options_.calls_file, std::ios::binary);
  if (!in) {
    return calls;  // no calls yet
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (!calls_offset_initialized_) {
    calls_offset_ = 0;
    calls_offset_initialized_ = true;
  }
  if (size < static_cast<std::streamoff>(calls_offset_)) {
    calls_offset_ = 0;  // file was truncated
  }
  in.seekg(static_cast<std::streamoff>(calls_offset_), std::ios::beg);
  std::string line;
  std::streamoff consumed = static_cast<std::streamoff>(calls_offset_);
  while (std::getline(in, line)) {
    consumed += static_cast<std::streamoff>(line.size()) + 1;
    if (line.empty()) {
      continue;
    }
    Json call = Json::parse(line, nullptr, false);
    if (!call.is_discarded()) {
      calls.push_back(std::move(call));
    }
  }
  calls_offset_ = static_cast<std::size_t>(consumed);
  return calls;
}

Result<AgentTurnOutcome> OpencodeAgentRunner::outcome_from_submission(
    const AgentTurnRequest& request, const Json& call) const {
  const std::string tool = call.value("tool", "");
  const Json arguments = call.value("arguments", Json::object());
  AgentTurnOutcome outcome;
  outcome.completed = true;
  if (tool == "submit_work") {
    outcome.summary = arguments.value("summary", "");
  } else if (tool == "submit_tests") {
    outcome.summary = arguments.value("summary", "");
  } else if (tool == "submit_review") {
    const Json verdict = arguments.value("verdict_json", Json::object());
    outcome.summary = verdict.value("summary", "review submitted");
    const auto findings = verdict.find("findings");
    if (findings != verdict.end() && findings->is_array() && !findings->empty()) {
      // Design section 8.2: only the configured severities block.
      outcome.changes_requested = false;
      for (const auto& finding : *findings) {
        if (!finding.is_object()) {
          continue;
        }
        const std::string severity = finding.value("severity", "");
        if (std::find(options_.blocking_severities.begin(),
                      options_.blocking_severities.end(),
                      severity) != options_.blocking_severities.end()) {
          outcome.changes_requested = true;
          break;
        }
      }
    } else {
      // A verdict without structured findings keeps the explicit verdict.
      outcome.changes_requested =
          verdict.value("verdict", "") == "CHANGES_REQUESTED";
    }
  } else if (tool == "dispute_test") {
    outcome.disputed_test_id = arguments.value("test_id", "");
    outcome.summary = "disputed test " + outcome.disputed_test_id;
  } else if (tool == "file_defect") {
    const Json defect = arguments.value("defect_json", Json::object());
    outcome.summary = defect.value("summary", "defect filed");
    outcome.exit_criteria_met = false;  // defects were filed
  } else if (tool == "verify_defect") {
    outcome.summary = "defects verified";
    outcome.exit_criteria_met = true;
  } else {
    return fail(error_codes::kUnexpectedSubmission,
                "unexpected submission tool '" + tool + "' for this phase");
  }
  if (request.phase == MilestonePhase::Validate && tool == "submit_tests") {
    outcome.exit_criteria_met = true;
  }
  return outcome;
}

Result<AgentTurnOutcome> OpencodeAgentRunner::drive(const AgentTurnRequest& request,
                                                    bool system_validation) {
  auto session = driver_.create_session();
  if (!session) {
    return tl::make_unexpected(session.error());
  }
  // Only calls that arrive after this point belong to this turn.
  auto priming = read_new_calls();
  if (!priming) {
    return tl::make_unexpected(priming.error());
  }

  const std::string prompt = build_prompt(request, system_validation);
  for (uint32_t attempt = 0; attempt <= options_.max_continuations; ++attempt) {
    const std::string& text = attempt == 0 ? prompt : options_.continuation_message;
    auto sent = driver_.send_prompt(*session, text);
    if (!sent) {
      return tl::make_unexpected(sent.error());
    }
    auto idle = driver_.wait_idle(*session, options_.turn_timeout,
                                  options_.stall_timeout);
    if (!idle) {
      (void)driver_.abort(*session);
      return fail(error_codes::kTurnTimeout,
                  "the turn did not finish within the watchdog timeout: " +
                      idle.error().message);
    }
    auto calls = read_new_calls();
    if (!calls) {
      return tl::make_unexpected(calls.error());
    }
    // A test dispute takes precedence over a work submission: the coder
    // cannot change a protected test itself, so QA decides before the next
    // gate run even when the same turn also submitted work (design section
    // 12). Otherwise the last accepted submission of the turn wins.
    for (auto it = calls->rbegin(); it != calls->rend(); ++it) {
      if (it->value("tool", "") != "dispute_test" ||
          !accepts_submission(request.phase, "dispute_test")) {
        continue;
      }
      auto outcome = outcome_from_submission(request, *it);
      if (outcome.has_value()) {
        record_usage(request, *session);
      }
      return outcome;
    }
    for (auto it = calls->rbegin(); it != calls->rend(); ++it) {
      const std::string tool = it->value("tool", "");
      if (!accepts_submission(request.phase, tool)) {
        continue;
      }
      auto outcome = outcome_from_submission(request, *it);
      if (outcome.has_value()) {
        record_usage(request, *session);
      }
      return outcome;
    }
  }
  return fail(error_codes::kNoSubmission,
              "the agent ended the turn without a structured submission after " +
                  std::to_string(options_.max_continuations + 1) + " attempts");
}

Result<AgentTurnOutcome> OpencodeAgentRunner::run_turn(const AgentTurnRequest& request) {
  return drive(request, false);
}

Result<AgentTurnOutcome> OpencodeAgentRunner::run_system_validation(
    const AgentTurnRequest& request) {
  AgentTurnRequest validation = request;
  validation.phase = MilestonePhase::Validate;
  if (validation.role.empty()) {
    validation.role = "qa";
  }
  return drive(validation, true);
}

}  // namespace cf::orch
