#include "cf/gates/runner.hpp"

#include <algorithm>
#include <set>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace cf::gates {
namespace {

using Json = nlohmann::json;

std::string summarize_output(const std::string& output, std::size_t max_lines = 20,
                             std::size_t max_chars = 2000) {
  if (output.empty()) {
    return {};
  }
  std::vector<std::string> lines;
  std::string current;
  for (const char c : output) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else if (c != '\r') {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  const std::size_t begin = lines.size() > max_lines ? lines.size() - max_lines : 0;
  std::string summary;
  for (std::size_t i = begin; i < lines.size(); ++i) {
    if (!summary.empty()) {
      summary.push_back('\n');
    }
    summary += lines[i];
  }
  if (summary.size() > max_chars) {
    summary = summary.substr(summary.size() - max_chars);
  }
  return summary;
}

int severity_rank(const std::string& severity) {
  if (severity == "error") {
    return 0;
  }
  if (severity == "warning") {
    return 1;
  }
  return 2;
}

GateResult run_gate(const GateDefinition& gate, ProcessExecutor& executor,
                    const GateRunOptions& options) {
  GateResult result;
  result.gate = gate.name;
  const auto start = std::chrono::steady_clock::now();

  result.passed = true;
  for (const auto& step : gate.steps) {
    auto expanded = expand_step(step, options.step_context);
    if (!expanded) {
      result.passed = false;
      result.findings.push_back(GateFinding{gate.name, "error", expanded.error().message});
      break;
    }
    ProcessRequest request;
    request.argv = *expanded;
    request.working_directory = options.working_directory;
    request.timeout = options.step_timeout;
    ProcessResult process = executor.run(request);

    if (!process.launched) {
      result.passed = false;
      result.findings.push_back(
          GateFinding{gate.name, "error",
                      "failed to launch '" + expanded->front() + "': " + process.error});
      break;
    }
    if (process.timed_out) {
      result.passed = false;
      result.findings.push_back(
          GateFinding{gate.name, "error",
                      "step timed out after " +
                          std::to_string(options.step_timeout.count()) + " ms: " +
                          expanded->front()});
      break;
    }
    if (process.exit_code != 0) {
      result.passed = false;
      std::string message = summarize_output(process.stderr_text);
      if (message.empty()) {
        message = summarize_output(process.stdout_text);
      }
      if (message.empty()) {
        message = "step exited with code " + std::to_string(process.exit_code) + ": " +
                  expanded->front();
      }
      result.findings.push_back(GateFinding{gate.name, "error", std::move(message)});
      break;
    }
  }

  result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  return result;
}

std::vector<GateResult> run_stage(const std::vector<const GateDefinition*>& gates,
                                  ProcessExecutor& executor,
                                  const GateRunOptions& options) {
  std::vector<GateResult> results(gates.size());
  std::vector<std::thread> threads;
  threads.reserve(gates.size());
  for (std::size_t i = 0; i < gates.size(); ++i) {
    threads.emplace_back([&results, &gates, &executor, &options, i] {
      results[i] = run_gate(*gates[i], executor, options);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  return results;
}

bool stage_passed(const std::vector<GateResult>& results) {
  return std::all_of(results.begin(), results.end(),
                     [](const GateResult& result) { return result.passed; });
}

GateResult skipped_gate(const GateDefinition& gate) {
  GateResult result;
  result.gate = gate.name;
  result.passed = false;
  result.skipped = true;
  return result;
}

void dedupe_and_cap(GateReport& report, const GateRunOptions& options) {
  std::set<std::string> seen;
  for (auto& result : report.results) {
    std::vector<GateFinding> kept;
    for (auto& finding : result.findings) {
      // Identical findings reported by more than one gate collapse to one.
      const std::string key = finding.severity + "\x1f" + finding.message;
      if (!seen.insert(key).second) {
        ++report.duplicate_findings_removed;
        continue;
      }
      kept.push_back(std::move(finding));
    }
    result.findings = std::move(kept);
    std::sort(result.findings.begin(), result.findings.end(),
              [](const GateFinding& a, const GateFinding& b) {
                const int rank_a = severity_rank(a.severity);
                const int rank_b = severity_rank(b.severity);
                if (rank_a != rank_b) {
                  return rank_a < rank_b;
                }
                return a.message < b.message;
              });
  }

  std::size_t total = 0;
  for (auto& result : report.results) {
    std::vector<GateFinding> kept;
    for (auto& finding : result.findings) {
      if (total >= options.max_findings ||
          kept.size() >= options.max_findings_per_gate) {
        ++report.findings_truncated;
        continue;
      }
      kept.push_back(std::move(finding));
      ++total;
    }
    result.findings = std::move(kept);
  }

  report.passed = std::all_of(report.results.begin(), report.results.end(),
                              [](const GateResult& result) { return result.passed; });
}

}  // namespace

std::string GateReport::to_json() const {
  Json json_results = Json::array();
  for (const auto& result : results) {
    Json findings = Json::array();
    for (const auto& finding : result.findings) {
      findings.push_back(Json{{"gate", finding.gate},
                              {"severity", finding.severity},
                              {"message", finding.message}});
    }
    json_results.push_back(Json{{"gate", result.gate},
                                {"passed", result.passed},
                                {"skipped", result.skipped},
                                {"duration_ms", result.duration_ms},
                                {"findings", std::move(findings)}});
  }
  const Json document = Json{{"commit", commit_sha},
                             {"passed", passed},
                             {"results", std::move(json_results)},
                             {"duplicate_findings_removed", duplicate_findings_removed},
                             {"findings_truncated", findings_truncated}};
  return document.dump();
}

GateRunner::GateRunner(ProcessExecutor& executor) : executor_(executor) {}

GateReport GateRunner::run(const std::vector<GateDefinition>& plan,
                           const GateRunOptions& options) const {
  GateReport report;
  report.commit_sha = options.commit_sha;

  std::vector<const GateDefinition*> prerequisites;
  std::vector<const GateDefinition*> cheap;
  std::vector<const GateDefinition*> expensive;
  for (const auto& gate : plan) {
    switch (gate.stage) {
      case GateStage::Prerequisites:
        prerequisites.push_back(&gate);
        break;
      case GateStage::Cheap:
        cheap.push_back(&gate);
        break;
      case GateStage::Expensive:
        expensive.push_back(&gate);
        break;
    }
  }

  bool stop = false;
  for (const auto* gate : prerequisites) {
    report.results.push_back(run_gate(*gate, executor_, options));
    if (!report.results.back().passed && gate->blocking) {
      stop = true;
    }
  }

  if (!stop) {
    std::vector<GateResult> stage = run_stage(cheap, executor_, options);
    const bool cheap_passed = stage_passed(stage);
    for (auto& result : stage) {
      report.results.push_back(std::move(result));
    }
    stop = !cheap_passed;
  } else {
    for (const auto* gate : cheap) {
      report.results.push_back(skipped_gate(*gate));
    }
  }

  if (!stop) {
    std::vector<GateResult> stage = run_stage(expensive, executor_, options);
    for (auto& result : stage) {
      report.results.push_back(std::move(result));
    }
  } else {
    for (const auto* gate : expensive) {
      report.results.push_back(skipped_gate(*gate));
    }
  }

  dedupe_and_cap(report, options);
  return report;
}

Result<std::vector<GateDefinition>> build_gate_plan(const Profile& profile,
                                                    std::string_view language,
                                                    std::string_view build_system,
                                                    std::string_view test_framework) {
  const auto language_it = profile.languages.find(language);
  if (language_it == profile.languages.end()) {
    return fail(error_codes::kNoMatchingToolchain,
                "profile '" + profile.id + "' has no language '" + std::string(language) +
                    "'");
  }
  const auto build_it = profile.build_systems.find(build_system);
  if (build_it == profile.build_systems.end()) {
    return fail(error_codes::kNoMatchingToolchain,
                "profile '" + profile.id + "' has no build system '" +
                    std::string(build_system) + "'");
  }
  const auto framework_it = profile.test_frameworks.find(test_framework);
  if (framework_it == profile.test_frameworks.end()) {
    return fail(error_codes::kNoMatchingToolchain,
                "profile '" + profile.id + "' has no test framework '" +
                    std::string(test_framework) + "'");
  }

  std::vector<GateDefinition> plan;
  plan.push_back(GateDefinition{"build", GateStage::Prerequisites, build_it->second.steps});
  plan.push_back(
      GateDefinition{"format", GateStage::Cheap, language_it->second.format.steps});
  plan.push_back(GateDefinition{"guardrails", GateStage::Cheap,
                                language_it->second.guardrails.steps});
  const auto analysis = profile.common.find("analysis");
  if (analysis != profile.common.end()) {
    plan.push_back(GateDefinition{"analysis", GateStage::Cheap, analysis->second.steps});
  }
  plan.push_back(
      GateDefinition{"tests", GateStage::Cheap, framework_it->second.steps});
  return plan;
}

}  // namespace cf::gates
