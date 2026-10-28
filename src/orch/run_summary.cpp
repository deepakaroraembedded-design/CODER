#include "cf/orch/run_summary.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cf::orch {
namespace {

using Json = nlohmann::json;

Json milestone_json(const store::MilestoneRecord& milestone) {
  return Json{{"id", milestone.milestone_id},
              {"state", milestone.state},
              {"iterations", milestone.iterations}};
}

Json gate_rows_json(const std::vector<store::GateResultRow>& rows) {
  Json gates = Json::array();
  for (const auto& row : rows) {
    gates.push_back(Json{{"gate", row.gate},
                         {"passed", row.passed},
                         {"commit_sha", row.commit_sha},
                         {"at", row.at}});
  }
  return gates;
}

Json defect_json(const store::DefectRecord& defect) {
  return Json{{"id", defect.id},
              {"milestone_id", defect.milestone_id},
              {"status", defect.status},
              {"severity", defect.severity},
              {"attempts", defect.attempts},
              {"body", Json::parse(defect.body_json, nullptr, false)}};
}

struct UsageTotals {
  double usd = 0.0;
  std::int64_t prompt_tokens = 0;
  std::int64_t completion_tokens = 0;
  std::int64_t cached_tokens = 0;

  void add(const UsageTotals& other) {
    usd += other.usd;
    prompt_tokens += other.prompt_tokens;
    completion_tokens += other.completion_tokens;
    cached_tokens += other.cached_tokens;
  }

  std::int64_t tokens() const { return prompt_tokens + completion_tokens; }
};

std::int64_t number_or_zero(const Json& object, const char* key) {
  const auto value = object.find(key);
  return value != object.end() && value->is_number_integer()
             ? value->get<std::int64_t>()
             : 0;
}

UsageTotals usage_totals(const Json& usage) {
  UsageTotals totals;
  if (!usage.is_object()) {
    return totals;
  }
  const auto cost = usage.find("cost");
  if (cost != usage.end() && cost->is_number()) {
    totals.usd = cost->get<double>();
  }
  const auto tokens = usage.find("tokens");
  if (tokens != usage.end() && tokens->is_object()) {
    totals.prompt_tokens = number_or_zero(*tokens, "input") +
                           number_or_zero(*tokens, "prompt");
    totals.completion_tokens = number_or_zero(*tokens, "output") +
                               number_or_zero(*tokens, "completion") +
                               number_or_zero(*tokens, "reasoning");
    const auto cache = tokens->find("cache");
    if (cache != tokens->end() && cache->is_object()) {
      totals.cached_tokens = number_or_zero(*cache, "read") +
                             number_or_zero(*cache, "write");
    }
  }
  totals.prompt_tokens += number_or_zero(usage, "prompt_tokens");
  totals.completion_tokens += number_or_zero(usage, "completion_tokens");
  totals.cached_tokens += number_or_zero(usage, "cached_tokens");
  return totals;
}

Json usage_rows(const std::map<std::string, UsageTotals, std::less<>>& rows,
                const char* key_name) {
  Json array = Json::array();
  for (const auto& [name, totals] : rows) {
    array.push_back(Json{{key_name, name},
                         {"usd", totals.usd},
                         {"prompt_tokens", totals.prompt_tokens},
                         {"completion_tokens", totals.completion_tokens},
                         {"cached_tokens", totals.cached_tokens},
                         {"tokens", totals.tokens()}});
  }
  return array;
}

void read_agent_usage(
    const std::filesystem::path& run_dir,
    std::map<std::string, UsageTotals, std::less<>>& by_role,
    std::map<std::string, UsageTotals, std::less<>>& by_milestone) {
  std::error_code ec;
  if (run_dir.empty() || !std::filesystem::is_directory(run_dir, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(run_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("usage-", 0) != 0 || entry.path().extension() != ".jsonl") {
      continue;
    }
    std::string file_role = name.substr(6, name.size() - 6 - 6);
    std::ifstream in(entry.path(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const Json parsed = Json::parse(line, nullptr, false);
      if (!parsed.is_object()) {
        continue;
      }
      const std::string role = parsed.value("role", file_role);
      std::string milestone = parsed.value("milestone_id", "");
      if (milestone.empty()) {
        milestone = "(run)";
      }
      const UsageTotals totals =
          usage_totals(parsed.value("usage", Json::object()));
      by_role[role.empty() ? "unknown" : role].add(totals);
      by_milestone[milestone].add(totals);
    }
  }
}

Json waiver_json(const store::WaiverRecord& waiver) {
  return Json{{"id", waiver.waiver_id},
              {"milestone_id", waiver.milestone_id},
              {"rule", waiver.rule},
              {"scope", waiver.scope},
              {"justification", waiver.justification},
              {"approved_by", waiver.approved_by},
              {"at", waiver.at}};
}

// One line of a Markdown table cell: pipes and newlines would break the row.
std::string markdown_cell(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '\n' || c == '\r') {
      out.push_back(' ');
    } else if (c == '|') {
      out += "\\|";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace

Result<RunSummary> build_run_summary(store::RunStore& store,
                                     std::string_view run_id,
                                     const plan::Plan* plan,
                                     const std::filesystem::path& run_dir) {
  auto run = store.get_run(run_id);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  auto milestones = store.list_milestones(run_id);
  auto usd = store.total_usd(run_id);
  auto tokens = store.total_tokens(run_id);
  auto defects = store.list_defects(run_id);
  auto waivers = store.list_waivers(run_id);
  auto waiver_requests = store.list_waiver_requests(run_id);
  if (!milestones || !usd || !tokens || !defects || !waivers ||
      !waiver_requests) {
    return fail("ORCH_REPORT_READ_FAILED", "cannot read the run store");
  }

  RunSummary summary;
  summary.run = *run;
  summary.usd = *usd;
  summary.tokens = *tokens;

  // Milestone rows with their latest gate results.
  std::map<std::string, bool, std::less<>> milestone_gates_passed;
  for (const auto& milestone : *milestones) {
    Json row = milestone_json(milestone);
    auto gates = store.latest_gate_results(run_id, milestone.milestone_id);
    row["gates"] = gates.has_value() ? gate_rows_json(*gates) : Json::array();
    bool passed = gates.has_value() && !gates->empty();
    if (gates.has_value()) {
      for (const auto& gate : *gates) {
        passed = passed && gate.passed;
      }
    } else {
      passed = false;
    }
    milestone_gates_passed[milestone.milestone_id] = passed;
    row["all_gates_passed"] = passed;
    summary.milestones.push_back(std::move(row));
  }

  // Requirement traceability (requirement -> milestones -> gate status).
  if (plan != nullptr) {
    std::map<std::string, std::vector<std::string>, std::less<>> by_requirement;
    for (const auto& spec : plan->milestones) {
      for (const auto& requirement : spec.requirements) {
        by_requirement[requirement].push_back(spec.id);
      }
    }
    for (const auto& [requirement, milestone_ids] : by_requirement) {
      bool done = true;
      bool gates_passed = true;
      Json ids = Json::array();
      for (const auto& id : milestone_ids) {
        ids.push_back(id);
        const auto state = std::find_if(
            milestones->begin(), milestones->end(),
            [&id](const store::MilestoneRecord& milestone) {
              return milestone.milestone_id == id;
            });
        done = done && state != milestones->end() && state->state == "Done";
        gates_passed = gates_passed && milestone_gates_passed[id];
      }
      summary.requirements.push_back(
          Json{{"requirement", requirement},
               {"milestones", std::move(ids)},
               {"milestones_done", done},
               {"gates_passed", gates_passed}});
    }
  }

  for (const auto& defect : *defects) {
    summary.defects.push_back(defect_json(defect));
  }
  for (const auto& waiver : *waivers) {
    summary.waivers.push_back(waiver_json(waiver));
  }
  for (const auto& request : *waiver_requests) {
    summary.waiver_requests.push_back(
        Json{{"id", request.id},
             {"milestone_id", request.milestone_id},
             {"rule", request.rule},
             {"scope", request.scope},
             {"justification", request.justification},
             {"requested_by", request.requested_by},
             {"status", request.status},
             {"requested_at", request.requested_at},
             {"deadline_at", request.deadline_at}});
  }

  // Agent-turn usage files live next to the run store. Their cost and tokens
  // are added to the ledger totals so the report reflects the whole run
  // (design sections 6.11, 14).
  std::map<std::string, UsageTotals, std::less<>> by_role;
  std::map<std::string, UsageTotals, std::less<>> by_milestone;
  read_agent_usage(run_dir, by_role, by_milestone);
  UsageTotals agent_totals;
  for (const auto& [role, totals] : by_role) {
    (void)role;
    agent_totals.add(totals);
  }
  summary.usd += agent_totals.usd;
  summary.tokens += agent_totals.tokens();
  summary.agent_usage_by_role = usage_rows(by_role, "role");
  summary.agent_usage_by_milestone = usage_rows(by_milestone, "milestone_id");
  return summary;
}

std::string render_run_summary_markdown(const RunSummary& summary) {
  std::string markdown = "## Milestones\n\n";
  markdown += "| id | state | iterations | gates |\n|---|---|---|---|\n";
  for (const auto& row : summary.milestones) {
    markdown += "| " + row["id"].get<std::string>() + " | " +
                row["state"].get<std::string>() + " | " +
                std::to_string(row["iterations"].get<unsigned>()) + " | " +
                (row["all_gates_passed"].get<bool>() ? "passed" : "not passed") +
                " |\n";
  }
  markdown += "\n## Requirement traceability\n\n";
  markdown += "| requirement | milestones | done | gates |\n|---|---|---|---|\n";
  for (const auto& row : summary.requirements) {
    std::string ids;
    for (const auto& id : row["milestones"]) {
      if (!ids.empty()) {
        ids += ", ";
      }
      ids += id.get<std::string>();
    }
    markdown += "| " + row["requirement"].get<std::string>() + " | " + ids +
                " | " + (row["milestones_done"].get<bool>() ? "yes" : "no") +
                " | " +
                (row["gates_passed"].get<bool>() ? "passed" : "not passed") +
                " |\n";
  }
  markdown += "\n## Defects\n\n";
  if (summary.defects.empty()) {
    markdown += "None.\n";
  } else {
    markdown += "| id | milestone | status | severity |\n|---|---|---|---|\n";
    for (const auto& row : summary.defects) {
      markdown += "| " + row["id"].get<std::string>() + " | " +
                  row["milestone_id"].get<std::string>() + " | " +
                  row["status"].get<std::string>() + " | " +
                  row["severity"].get<std::string>() + " |\n";
    }
  }
  markdown += "\n## Waivers\n\n";
  if (summary.waivers.empty()) {
    markdown += "None.\n";
  } else {
    markdown +=
        "| rule | scope | milestone | approved by | justification |\n"
        "|---|---|---|---|---|\n";
    for (const auto& row : summary.waivers) {
      std::string milestone = row["milestone_id"].get<std::string>();
      if (milestone.empty()) {
        milestone = "all";
      }
      markdown += "| " + markdown_cell(row["rule"].get<std::string>()) + " | " +
                  markdown_cell(row["scope"].get<std::string>()) + " | " +
                  markdown_cell(milestone) + " | " +
                  markdown_cell(row["approved_by"].get<std::string>()) + " | " +
                  markdown_cell(row["justification"].get<std::string>()) + " |\n";
    }
  }
  markdown += "\n## Waiver requests\n\n";
  if (summary.waiver_requests.empty()) {
    markdown += "None.\n";
  } else {
    markdown +=
        "| id | milestone | rule | scope | requested by | status | deadline |\n"
        "|---|---|---|---|---|---|---|\n";
    for (const auto& row : summary.waiver_requests) {
      std::string milestone = row["milestone_id"].get<std::string>();
      if (milestone.empty()) {
        milestone = "all";
      }
      markdown += "| " + markdown_cell(row["id"].get<std::string>()) + " | " +
                  markdown_cell(milestone) + " | " +
                  markdown_cell(row["rule"].get<std::string>()) + " | " +
                  markdown_cell(row["scope"].get<std::string>()) + " | " +
                  markdown_cell(row["requested_by"].get<std::string>()) + " | " +
                  markdown_cell(row["status"].get<std::string>()) + " | " +
                  std::to_string(row["deadline_at"].get<std::int64_t>()) + " |\n";
    }
  }
  markdown += "\n## Agent usage\n\n";
  if (summary.agent_usage_by_role.empty() &&
      summary.agent_usage_by_milestone.empty()) {
    markdown += "No agent-turn usage recorded.\n";
  } else {
    markdown +=
        "| role | prompt tokens | completion tokens | cached tokens | USD |\n"
        "|---|---|---|---|---|\n";
    for (const auto& row : summary.agent_usage_by_role) {
      markdown += "| " + markdown_cell(row["role"].get<std::string>()) + " | " +
                  std::to_string(row["prompt_tokens"].get<std::int64_t>()) +
                  " | " +
                  std::to_string(row["completion_tokens"].get<std::int64_t>()) +
                  " | " +
                  std::to_string(row["cached_tokens"].get<std::int64_t>()) +
                  " | " + std::to_string(row["usd"].get<double>()) + " |\n";
    }
    markdown +=
        "\n| milestone | prompt tokens | completion tokens | cached tokens | "
        "USD |\n|---|---|---|---|---|\n";
    for (const auto& row : summary.agent_usage_by_milestone) {
      markdown +=
          "| " + markdown_cell(row["milestone_id"].get<std::string>()) + " | " +
          std::to_string(row["prompt_tokens"].get<std::int64_t>()) + " | " +
          std::to_string(row["completion_tokens"].get<std::int64_t>()) + " | " +
          std::to_string(row["cached_tokens"].get<std::int64_t>()) + " | " +
          std::to_string(row["usd"].get<double>()) + " |\n";
    }
  }
  return markdown;
}

}  // namespace cf::orch
