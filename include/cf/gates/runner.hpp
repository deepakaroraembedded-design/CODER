// cf-gates: staged gate runner (design section 6.7).
//
// Gates run in three stages: prerequisites (stop on failure), cheap
// independent checks (run in parallel, all results aggregated) and expensive
// checks (only when the earlier stages passed). The aggregated report removes
// duplicate findings, sorts them by severity and gate, and is capped in size
// for the prompt; the full report stays in the run store.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/gates/process.hpp"
#include "cf/gates/profile.hpp"

namespace cf::gates {

enum class GateStage { Prerequisites = 1, Cheap = 2, Expensive = 3 };

struct GateDefinition {
  std::string name;
  GateStage stage = GateStage::Cheap;
  std::vector<GateStep> steps;  // trusted steps, executed as argument lists
  bool blocking = true;
};

struct GateFinding {
  std::string gate;
  std::string severity;  // "error", "warning", "note"
  std::string message;
};

struct GateResult {
  std::string gate;
  bool passed = false;
  bool skipped = false;  // an earlier stage failed
  int64_t duration_ms = 0;
  std::vector<GateFinding> findings;
};

struct GateReport {
  std::string commit_sha;
  bool passed = false;
  std::vector<GateResult> results;  // plan order
  std::size_t duplicate_findings_removed = 0;
  std::size_t findings_truncated = 0;

  // Structured JSON for the run store and the next agent prompt.
  std::string to_json() const;
};

struct GateRunOptions {
  StepContext step_context;
  std::string working_directory;
  std::string commit_sha;
  std::chrono::milliseconds step_timeout{600000};
  std::size_t max_findings = 200;          // prompt cap (design section 6.7)
  std::size_t max_findings_per_gate = 20;
};

class GateRunner {
 public:
  explicit GateRunner(ProcessExecutor& executor);

  // Stage 1 gates run sequentially; a failure skips every later stage.
  // Stage 2 gates run in parallel and are all aggregated. Stage 3 runs only
  // when every stage 1 and stage 2 gate passed.
  GateReport run(const std::vector<GateDefinition>& plan,
                 const GateRunOptions& options) const;

 private:
  ProcessExecutor& executor_;
};

// Builds the standard staged gate plan from a profile (design section 6.7):
//   stage 1: build
//   stage 2: format, guardrails, analysis (when declared), tests
// The orchestrator adds the allowed-paths gate (a diff check, not a
// subprocess) and the cf-codeintel architecture gate. Stage 3 gates
// (sanitizers, coverage, mutation, fuzzing) are added when their runners
// land; they are tool configurations rather than plain steps.
Result<std::vector<GateDefinition>> build_gate_plan(const Profile& profile,
                                                    std::string_view language,
                                                    std::string_view build_system,
                                                    std::string_view test_framework);

}  // namespace cf::gates
