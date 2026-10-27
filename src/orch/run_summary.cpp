#include "cf/orch/run_summary.hpp"

#include <algorithm>
#include <map>
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

}  // namespace

Result<RunSummary> build_run_summary(store::RunStore& store,
                                     std::string_view run_id,
                                     const plan::Plan* plan) {
  auto run = store.get_run(run_id);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  auto milestones = store.list_milestones(run_id);
  auto usd = store.total_usd(run_id);
  auto tokens = store.total_tokens(run_id);
  auto defects = store.list_defects(run_id);
  if (!milestones || !usd || !tokens || !defects) {
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
  return markdown;
}

}  // namespace cf::orch
