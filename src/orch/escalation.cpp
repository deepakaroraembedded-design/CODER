#include "cf/orch/escalation.hpp"

#include <fstream>
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
  auto transitions = store.list_transitions(run_id, 20);
  if (!transitions) {
    return tl::make_unexpected(transitions.error());
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

  const Json bundle = Json{{"run_id", std::string(run_id)},
                           {"reason", std::string(reason)},
                           {"at", now},
                           {"run_state", run->state},
                           {"plan_id", run->plan_id},
                           {"milestones", std::move(json_milestones)},
                           {"operations", std::move(json_operations)},
                           {"recent_transitions", std::move(json_transitions)},
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
