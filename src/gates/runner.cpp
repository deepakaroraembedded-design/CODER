#include "cf/gates/runner.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
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

Result<CoverageTotals> parse_coverage(std::string_view format,
                                      std::string_view content) {
  CoverageTotals totals;
  if (format == "go-cover") {
    std::uint64_t statements = 0;
    std::uint64_t covered = 0;
    std::istringstream lines{std::string(content)};
    for (std::string line; std::getline(lines, line);) {
      const std::size_t space = line.rfind(' ');
      if (space == std::string::npos) {
        continue;
      }
      const std::size_t previous = line.rfind(' ', space - 1);
      if (previous == std::string::npos) {
        continue;
      }
      try {
        const std::uint64_t count = std::stoull(line.substr(space + 1));
        const std::uint64_t statements_in_line =
            std::stoull(line.substr(previous + 1, space - previous - 1));
        statements += statements_in_line;
        if (count > 0) {
          covered += statements_in_line;
        }
      } catch (const std::exception&) {
        continue;
      }
    }
    totals.line = statements == 0 ? 1.0
                                  : static_cast<double>(covered) / statements;
    return totals;
  }
  if (format == "coverage-json") {
    const Json document = Json::parse(content.data(), content.data() + content.size(),
                                     nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return fail(error_codes::kCoverageReport, "coverage JSON is invalid");
    }
    const auto totals_it = document.find("totals");
    if (totals_it == document.end() || !totals_it->is_object()) {
      return fail(error_codes::kCoverageReport, "coverage JSON has no totals");
    }
    const auto number = [&totals_it](const char* key) -> double {
      const auto it = totals_it->find(key);
      return it != totals_it->end() && it->is_number() ? it->get<double>() : 0.0;
    };
    const double statements = number("num_statements");
    const double covered_lines = number("covered_lines");
    totals.line = statements == 0.0 ? 1.0 : covered_lines / statements;
    const double branches = number("num_branches");
    const double covered_branches = number("covered_branches");
    totals.has_branch = branches > 0.0;
    totals.branch = !totals.has_branch ? 0.0 : covered_branches / branches;
    return totals;
  }
  if (format == "llvm-cov-json") {
    const Json document = Json::parse(content.data(), content.data() + content.size(),
                                     nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return fail(error_codes::kCoverageReport, "llvm-cov JSON is invalid");
    }
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array() || data->empty()) {
      return fail(error_codes::kCoverageReport, "llvm-cov JSON has no data");
    }
    const auto totals_it = data->front().find("totals");
    if (totals_it == data->front().end() || !totals_it->is_object()) {
      return fail(error_codes::kCoverageReport, "llvm-cov JSON has no totals");
    }
    const auto counter = [&totals_it](const char* key, std::uint64_t& covered,
                                      std::uint64_t& count) -> bool {
      const auto node = totals_it->find(key);
      if (node == totals_it->end() || !node->is_object()) {
        return false;
      }
      covered = node->value("covered", std::uint64_t{0});
      count = node->value("count", std::uint64_t{0});
      return true;
    };
    std::uint64_t covered = 0;
    std::uint64_t count = 0;
    if (!counter("lines", covered, count)) {
      return fail(error_codes::kCoverageReport, "llvm-cov JSON has no line totals");
    }
    totals.line = count == 0 ? 1.0 : static_cast<double>(covered) / count;
    if (counter("branches", covered, count)) {
      totals.has_branch = count > 0;
      totals.branch = count == 0 ? 0.0 : static_cast<double>(covered) / count;
    }
    return totals;
  }
  if (format == "jacoco-xml") {
    std::uint64_t line_missed = 0;
    std::uint64_t line_covered = 0;
    std::uint64_t branch_missed = 0;
    std::uint64_t branch_covered = 0;
    bool saw_branch = false;
    const std::string text(content);
    std::size_t position = 0;
    while ((position = text.find("<counter ", position)) != std::string::npos) {
      const std::size_t end = text.find("/>", position);
      if (end == std::string::npos) {
        break;
      }
      const std::string tag = text.substr(position, end - position);
      const auto attribute = [&tag](const std::string& name) -> std::uint64_t {
        const std::string needle = name + "=\"";
        const std::size_t at = tag.find(needle);
        if (at == std::string::npos) {
          return 0;
        }
        const std::size_t begin = at + needle.size();
        const std::size_t close = tag.find('"', begin);
        if (close == std::string::npos) {
          return 0;
        }
        try {
          return std::stoull(tag.substr(begin, close - begin));
        } catch (const std::exception&) {
          return 0;
        }
      };
      if (tag.find("type=\"LINE\"") != std::string::npos) {
        line_missed += attribute("missed");
        line_covered += attribute("covered");
      } else if (tag.find("type=\"BRANCH\"") != std::string::npos) {
        branch_missed += attribute("missed");
        branch_covered += attribute("covered");
        saw_branch = true;
      }
      position = end;
    }
    const std::uint64_t line_total = line_missed + line_covered;
    totals.line = line_total == 0 ? 1.0
                                  : static_cast<double>(line_covered) / line_total;
    totals.has_branch = saw_branch;
    const std::uint64_t branch_total = branch_missed + branch_covered;
    totals.branch = !saw_branch || branch_total == 0
                        ? 0.0
                        : static_cast<double>(branch_covered) / branch_total;
    if (saw_branch && branch_total == 0) {
      totals.branch = 1.0;
    }
    return totals;
  }
  if (format == "lcov") {
    std::uint64_t lines_found = 0;
    std::uint64_t lines_hit = 0;
    std::uint64_t branches_found = 0;
    std::uint64_t branches_hit = 0;
    bool saw_branch_record = false;
    std::istringstream stream{std::string(content)};
    for (std::string line; std::getline(stream, line);) {
      const auto number = [&line](std::string_view prefix) -> std::uint64_t {
        if (line.rfind(prefix, 0) != 0) {
          return 0;
        }
        try {
          return std::stoull(line.substr(prefix.size()));
        } catch (const std::exception&) {
          return 0;
        }
      };
      if (line.rfind("LF:", 0) == 0) {
        lines_found += number("LF:");
      } else if (line.rfind("LH:", 0) == 0) {
        lines_hit += number("LH:");
      } else if (line.rfind("BRF:", 0) == 0) {
        branches_found += number("BRF:");
        saw_branch_record = true;
      } else if (line.rfind("BRH:", 0) == 0) {
        branches_hit += number("BRH:");
        saw_branch_record = true;
      }
    }
    totals.line = lines_found == 0 ? 1.0
                                   : static_cast<double>(lines_hit) / lines_found;
    totals.has_branch = saw_branch_record;
    if (saw_branch_record) {
      totals.branch = branches_found == 0
                          ? 1.0
                          : static_cast<double>(branches_hit) / branches_found;
    }
    return totals;
  }
  return fail(error_codes::kCoverageReport,
              "unknown coverage report format '" + std::string(format) + "'");
}

void check_coverage(const GateDefinition& gate, const GateRunOptions& options,
                    GateResult& result) {
  if (gate.coverage.format.empty() || gate.coverage.report.empty()) {
    return;
  }
  GateStep report_step;
  report_step.argv = {gate.coverage.report};
  auto expanded = expand_step(report_step, options.step_context);
  if (!expanded || expanded->empty()) {
    result.passed = false;
    result.findings.push_back(
        GateFinding{gate.name, "error", "cannot expand the coverage report path"});
    return;
  }
  std::filesystem::path path = expanded->front();
  if (path.is_relative()) {
    path = std::filesystem::path(options.working_directory) / path;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.passed = false;
    result.findings.push_back(GateFinding{
        gate.name, "error", "cannot read the coverage report " + path.string()});
    return;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  auto totals = parse_coverage(gate.coverage.format, buffer.str());
  if (!totals) {
    result.passed = false;
    result.findings.push_back(
        GateFinding{gate.name, "error", totals.error().message});
    return;
  }
  if (gate.coverage.line > 0.0 && totals->line + 1e-9 < gate.coverage.line) {
    result.passed = false;
    result.findings.push_back(
        GateFinding{gate.name, "error",
                    "line coverage " + std::to_string(totals->line) +
                        " is below " + std::to_string(gate.coverage.line)});
  }
  if (gate.coverage.branch > 0.0) {
    if (!totals->has_branch) {
      result.passed = false;
      result.findings.push_back(GateFinding{
          gate.name, "error", "the coverage report has no branch data"});
    } else if (totals->branch + 1e-9 < gate.coverage.branch) {
      result.passed = false;
      result.findings.push_back(
          GateFinding{gate.name, "error",
                      "branch coverage " + std::to_string(totals->branch) +
                          " is below " + std::to_string(gate.coverage.branch)});
    }
  }
}

Result<MutationTotals> parse_mutation(std::string_view format,
                                      std::string_view content) {
  if (format == "mutmut-json") {
    const Json document = Json::parse(content.data(), content.data() + content.size(),
                                     nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return fail(error_codes::kMutationReport, "mutation JSON is invalid");
    }
    const double total = document.value("total", 0.0);
    const double killed = document.value("killed", 0.0);
    MutationTotals totals;
    totals.score = total == 0.0 ? 1.0 : killed / total;
    return totals;
  }
  return fail(error_codes::kMutationReport,
              "unknown mutation report format '" + std::string(format) + "'");
}

void check_mutation(const GateDefinition& gate, const GateRunOptions& options,
                    GateResult& result) {
  if (gate.mutation.format.empty() || gate.mutation.report.empty()) {
    return;
  }
  GateStep report_step;
  report_step.argv = {gate.mutation.report};
  auto expanded = expand_step(report_step, options.step_context);
  if (!expanded || expanded->empty()) {
    result.passed = false;
    result.findings.push_back(
        GateFinding{gate.name, "error", "cannot expand the mutation report path"});
    return;
  }
  std::filesystem::path path = expanded->front();
  if (path.is_relative()) {
    path = std::filesystem::path(options.working_directory) / path;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.passed = false;
    result.findings.push_back(GateFinding{
        gate.name, "error", "cannot read the mutation report " + path.string()});
    return;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  auto totals = parse_mutation(gate.mutation.format, buffer.str());
  if (!totals) {
    result.passed = false;
    result.findings.push_back(
        GateFinding{gate.name, "error", totals.error().message});
    return;
  }
  if (gate.mutation.min_score > 0.0 &&
      totals->score + 1e-9 < gate.mutation.min_score) {
    result.passed = false;
    result.findings.push_back(
        GateFinding{gate.name, "error",
                    "mutation score " + std::to_string(totals->score) +
                        " is below " + std::to_string(gate.mutation.min_score)});
  }
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

  if (result.passed) {
    check_coverage(gate, options, result);
    check_mutation(gate, options, result);
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
                                {"waived", result.waived},
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

Result<CoverageTotals> parse_coverage_report(std::string_view format,
                                             std::string_view content) {
  return parse_coverage(format, content);
}

Result<MutationTotals> parse_mutation_report(std::string_view format,
                                             std::string_view content) {
  return parse_mutation(format, content);
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
  plan.push_back(GateDefinition{"build", GateStage::Prerequisites, build_it->second.steps, true, {}, {}});
  plan.push_back(
      GateDefinition{"format", GateStage::Cheap, language_it->second.format.steps, true, {}, {}});
  plan.push_back(GateDefinition{"guardrails", GateStage::Cheap,
                                language_it->second.guardrails.steps, true, {}, {}});
  const auto analysis = profile.common.find("analysis");
  if (analysis != profile.common.end()) {
    plan.push_back(GateDefinition{"analysis", GateStage::Cheap, analysis->second.steps, true, {}, {}});
  }
  plan.push_back(
      GateDefinition{"tests", GateStage::Cheap, framework_it->second.steps, true, {}, {}});
  // Stage 3 runs only when every prerequisite and cheap gate passed. A
  // profile without steps has no coverage/mutation runner configured.
  if (!profile.coverage.steps.empty()) {
    plan.push_back(GateDefinition{
        "coverage", GateStage::Expensive, profile.coverage.steps, true,
        CoverageCheck{profile.coverage.format, profile.coverage.report,
                      profile.coverage.line, profile.coverage.branch},
        {}});
  }
  if (!profile.mutation.steps.empty()) {
    plan.push_back(GateDefinition{
        "mutation", GateStage::Expensive, profile.mutation.steps, true, {},
        MutationCheck{profile.mutation.format, profile.mutation.report,
                      profile.mutation.min_score}});
  }
  return plan;
}

}  // namespace cf::gates
