// Phase 0a harness compatibility spike: CLI and report (design section 6.6.2).
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>
#include <thread>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include "harness.hpp"

namespace {

std::string now_iso() {
  const std::time_t now = std::time(nullptr);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
  return buffer;
}

std::string now_stamp() {
  const std::time_t now = std::time(nullptr);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", std::localtime(&now));
  return buffer;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : text) {
    if (c == delimiter) {
      if (!current.empty()) {
        parts.push_back(current);
      }
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

}  // namespace

int main(int argc, char** argv) {
  cf::spike::SpikeOptions options;
  options.workdir = "spike-out";

  CLI::App app{"Phase 0a opencode harness compatibility spike (scenarios H1-H15)"};
  app.add_option("--opencode", options.opencode_binary,
                 "opencode binary (default: from PATH)");
  app.add_option("--mcp", options.mcp_binary, "path to the cf-mcp binary")
      ->required();
  app.add_option("--provider", options.provider_id, "model provider ID");
  app.add_option("--model", options.model_id, "coder model ID");
  app.add_option("--reviewer-model", options.reviewer_model_id, "reviewer model ID");
  app.add_option("--qa-model", options.qa_model_id, "QA model ID");
  app.add_option("--workdir", options.workdir, "working directory for run artifacts");
  app.add_option("--runs", options.runs, "iterations per scenario");
  app.add_option("--timeout", options.timeout_s, "per-iteration budget in seconds");
  std::string scenario_list;
  app.add_option("--scenarios", scenario_list,
                 "comma-separated scenario list (default: all)");
  app.add_flag("--keep-servers", options.keep_servers,
               "do not kill opencode servers on exit (debugging)");
  CLI11_PARSE(app, argc, argv);

  std::error_code ec;
  std::filesystem::create_directories(options.workdir, ec);

  auto registry = cf::spike::scenario_registry();
  std::vector<std::string> selected;
  if (!scenario_list.empty()) {
    selected = split(scenario_list, ',');
  } else {
    for (const auto& [name, fn] : registry) {
      selected.push_back(name);
    }
    std::sort(selected.begin(), selected.end());
  }

  nlohmann::json report;
  report["started_at"] = now_iso();
  report["opencode_binary"] = options.opencode_binary;
  report["options"] = nlohmann::json{{"provider", options.provider_id},
                                     {"model", options.model_id},
                                     {"reviewer_model", options.reviewer_model_id},
                                     {"qa_model", options.qa_model_id},
                                     {"runs", options.runs},
                                     {"timeout_s", options.timeout_s},
                                     {"workdir", options.workdir.string()}};
  nlohmann::json results = nlohmann::json::array();

  int total = 0;
  int passed = 0;
  for (const auto& name : selected) {
    const auto it = registry.find(name);
    if (it == registry.end()) {
      spdlog::error("unknown scenario: {}", name);
      return 2;
    }
    for (int iteration = 1; iteration <= options.runs; ++iteration) {
      spdlog::info("running {} iteration {}/{}", name, iteration, options.runs);
      cf::spike::IterationResult result;
      try {
        result = it->second(options, iteration);
      } catch (const std::exception& ex) {
        result.scenario = name;
        result.iteration = iteration;
        result.pass = false;
        result.detail = std::string("unhandled exception: ") + ex.what();
      }
      ++total;
      if (result.pass) {
        ++passed;
      }
      spdlog::info("  {} {}: {}", name, result.pass ? "PASS" : "FAIL",
                   result.detail);
      results.push_back(nlohmann::json{{"scenario", result.scenario},
                                       {"iteration", result.iteration},
                                       {"pass", result.pass},
                                       {"detail", result.detail},
                                       {"evidence", result.evidence}});
    }
  }

  // Aggregate per scenario.
  nlohmann::json by_scenario = nlohmann::json::object();
  nlohmann::json h12_models = nlohmann::json::object();
  for (const auto& result : results) {
    const std::string scenario = result.value("scenario", "");
    if (!by_scenario.contains(scenario)) {
      by_scenario[scenario] = nlohmann::json{{"runs", 0}, {"passed", 0}};
    }
    by_scenario[scenario]["runs"] = by_scenario[scenario]["runs"].get<int>() + 1;
    if (result.value("pass", false)) {
      by_scenario[scenario]["passed"] =
          by_scenario[scenario]["passed"].get<int>() + 1;
    }
    if (scenario == "H12" && result.contains("evidence") &&
        result["evidence"].contains("per_model")) {
      for (auto it = result["evidence"]["per_model"].begin();
           it != result["evidence"]["per_model"].end(); ++it) {
        if (!h12_models.contains(it.key())) {
          h12_models[it.key()] = nlohmann::json{{"tasks", 0}, {"succeeded", 0}};
        }
        h12_models[it.key()]["tasks"] =
            h12_models[it.key()]["tasks"].get<int>() + 1;
        if (it.value().value("ok", false)) {
          h12_models[it.key()]["succeeded"] =
              h12_models[it.key()]["succeeded"].get<int>() + 1;
        }
      }
    }
  }

  report["finished_at"] = now_iso();
  report["total_iterations"] = total;
  report["passed_iterations"] = passed;
  report["by_scenario"] = by_scenario;
  report["h12_tool_calling"] = h12_models;
  report["results"] = results;

  const auto report_path =
      options.workdir / ("report-" + now_stamp() + ".json");
  {
    std::ofstream out(report_path);
    out << report.dump(2) << "\n";
  }

  spdlog::info("==================================================");
  spdlog::info("spike finished: {}/{} iterations passed", passed, total);
  for (auto it = by_scenario.begin(); it != by_scenario.end(); ++it) {
    spdlog::info("  {}: {}/{}", it.key(), it.value()["passed"].get<int>(),
                 it.value()["runs"].get<int>());
  }
  if (!h12_models.empty()) {
    spdlog::info("H12 tool-calling success by model family:");
    for (auto it = h12_models.begin(); it != h12_models.end(); ++it) {
      spdlog::info("  {}: {}/{}", it.key(), it.value()["succeeded"].get<int>(),
                   it.value()["tasks"].get<int>());
    }
  }
  spdlog::info("report: {}", report_path.string());
  return passed == total ? 0 : 1;
}
