#include "cf/orch/escalation.hpp"

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace cf::orch {
namespace {

using Json = nlohmann::json;

Json milestone_json(const store::MilestoneRecord& milestone) {
  return Json{{"id", milestone.milestone_id},
              {"state", milestone.state},
              {"iterations", milestone.iterations}};
}

Json operation_json(const store::OperationRecord& operation) {
  return Json{{"op_id", operation.op_id},
              {"kind", operation.kind},
              {"state", operation.state},
              {"milestone_id", operation.milestone_id},
              {"attempts", operation.attempts},
              {"result", operation.result_json}};
}

Json transition_json(const store::TransitionRecord& transition) {
  return Json{{"from", transition.from_state},
              {"to", transition.to_state},
              {"event", transition.event_json},
              {"at", transition.at}};
}

Json defect_json(const store::DefectRecord& defect) {
  return Json{{"id", defect.id},
              {"milestone_id", defect.milestone_id},
              {"status", defect.status},
              {"severity", defect.severity},
              {"attempts", defect.attempts},
              {"fingerprint", defect.fingerprint},
              {"body", Json::parse(defect.body_json, nullptr, false)}};
}

// The failing findings of a gate result, read from its report.json (design
// section 12: the bundle carries the gate logs/findings).
Json gate_json(const store::GateResultRow& row) {
  Json entry{{"gate", row.gate},
             {"milestone_id", row.milestone_id},
             {"passed", row.passed},
             {"report_ref", row.report_ref}};
  std::ifstream in(row.report_ref, std::ios::binary);
  if (in) {
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const Json report = Json::parse(text, nullptr, false);
    if (report.is_object()) {
      Json findings = Json::array();
      for (const auto& result : report.value("results", Json::array())) {
        if (result.value("passed", true)) {
          continue;
        }
        findings.push_back(Json{{"gate", result.value("gate", "")},
                                {"findings", result.value("findings",
                                                          Json::array())}});
      }
      entry["failing_results"] = std::move(findings);
    }
  }
  return entry;
}

}  // namespace

Result<std::string> write_escalation_bundle(store::RunStore& store,
                                            std::string_view run_id,
                                            const std::filesystem::path& dir,
                                            std::string_view reason,
                                            store::UnixTime now) {
  const auto run = store.get_run(run_id);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  auto milestones = store.list_milestones(run_id);
  if (!milestones) {
    return tl::make_unexpected(milestones.error());
  }
  auto operations = store.list_operations(run_id, {});
  if (!operations) {
    return tl::make_unexpected(operations.error());
  }
  auto transitions = store.list_transitions(run_id, 50);
  if (!transitions) {
    return tl::make_unexpected(transitions.error());
  }
  auto defects = store.list_defects(run_id);
  if (!defects) {
    return tl::make_unexpected(defects.error());
  }
  auto gate_rows = store.list_all_gate_results(run_id);
  if (!gate_rows) {
    return tl::make_unexpected(gate_rows.error());
  }
  auto waiver_requests = store.list_waiver_requests(run_id);
  if (!waiver_requests) {
    return tl::make_unexpected(waiver_requests.error());
  }
  auto usd = store.total_usd(run_id);
  if (!usd) {
    return tl::make_unexpected(usd.error());
  }
  auto tokens = store.total_tokens(run_id);
  if (!tokens) {
    return tl::make_unexpected(tokens.error());
  }

  Json json_milestones = Json::array();
  for (const auto& milestone : *milestones) {
    json_milestones.push_back(milestone_json(milestone));
  }
  Json json_operations = Json::array();
  for (const auto& operation : *operations) {
    json_operations.push_back(operation_json(operation));
  }
  Json json_transitions = Json::array();
  for (const auto& transition : *transitions) {
    json_transitions.push_back(transition_json(transition));
  }

  Json json_defects = Json::array();
  std::string root_cause = "see the failing gates and defects";
  for (const auto& defect : *defects) {
    json_defects.push_back(defect_json(defect));
    if (root_cause == "see the failing gates and defects" &&
        defect.status != "VERIFIED" && defect.status != "WONT_FIX") {
      root_cause = "open defect " + defect.id + " in milestone " +
                   defect.milestone_id;
    }
  }
  Json json_gates = Json::array();
  for (const auto& row : *gate_rows) {
    if (!row.passed) {
      json_gates.push_back(gate_json(row));
      if (root_cause == "see the failing gates and defects") {
        root_cause = "gate '" + row.gate + "' failed for milestone " +
                     row.milestone_id;
      }
    }
  }
  Json json_waivers = Json::array();
  for (const auto& request : *waiver_requests) {
    json_waivers.push_back(Json{{"id", request.id},
                                {"rule", request.rule},
                                {"scope", request.scope},
                                {"status", request.status},
                                {"requested_by", request.requested_by}});
  }

  const Json bundle = Json{{"run_id", std::string(run_id)},
                           {"reason", std::string(reason)},
                           {"at", now},
                           {"run_state", run->state},
                           {"plan_id", run->plan_id},
                           {"milestones", std::move(json_milestones)},
                           {"operations", std::move(json_operations)},
                           {"recent_transitions", std::move(json_transitions)},
                           {"defects", std::move(json_defects)},
                           {"failing_gates", std::move(json_gates)},
                           {"waiver_requests", std::move(json_waivers)},
                           {"likely_root_cause", root_cause},
                           {"root_cause_source", "deterministic"},
                           {"cost", Json{{"usd", *usd}, {"tokens", *tokens}}}};

  const std::filesystem::path directory = dir / ("esc-" + std::to_string(now));
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return fail(error_codes::kBundleWriteFailed,
                "cannot create escalation directory " + directory.string() + ": " +
                    ec.message());
  }
  const std::filesystem::path path = directory / "bundle.json";
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return fail(error_codes::kBundleWriteFailed,
                "cannot write escalation bundle: " + path.string());
  }
  out << bundle.dump(2) << "\n";
  if (!out.good()) {
    return fail(error_codes::kBundleWriteFailed,
                "cannot write escalation bundle: " + path.string());
  }
  return path.string();
}

}  // namespace cf::orch
