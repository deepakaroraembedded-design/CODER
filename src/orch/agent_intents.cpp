#include "cf/orch/agent_intents.hpp"

#include <algorithm>
#include <utility>

#include "cf/common/hash.hpp"
#include "cf/orch/approval_deadline.hpp"

namespace cf::orch {
namespace {

using Json = nlohmann::json;

std::string next_defect_id(store::RunStore& store, std::string_view run_id) {
  auto all = store.list_defects(run_id);
  const std::size_t count = all.has_value() ? all->size() : 0;
  return "D-" + std::to_string(count + 1);
}

std::string mapped_id(const std::map<std::string, std::string>& id_map,
                      const std::string& id) {
  const auto found = id_map.find(id);
  return found == id_map.end() ? id : found->second;
}

Status apply_file_defect(store::RunStore& store, std::string_view run_id,
                         std::string_view milestone_id, const Json& call,
                         std::map<std::string, std::string>& id_map) {
  const Json arguments = call.value("arguments", Json::object());
  if (!arguments.contains("defect_json") ||
      !arguments.at("defect_json").is_object()) {
    return fail("ORCH_INVALID_INTENT", "file_defect without a defect payload");
  }
  const Json& defect = arguments.at("defect_json");
  std::string fingerprint = defect.value("fingerprint", "");
  if (fingerprint.empty()) {
    fingerprint = cf::sha256_hex(defect.value("test_id", "") + "\n" +
                                 defect.value("summary", ""));
  }
  const std::string milestone =
      defect.value("milestone_id", std::string(milestone_id));
  const std::string severity = defect.value("severity", "medium");

  auto existing = store.find_defect_by_fingerprint(run_id, fingerprint);
  if (existing.has_value()) {
    store::DefectRecord record = *existing;
    record.milestone_id = milestone;
    record.severity = severity;
    record.body_json = defect.dump();
    record.status = "REOPENED";
    auto stored = store.upsert_defect(record);
    if (!stored) {
      return stored;
    }
    id_map[record.id] = record.id;
    const std::string reported = call.value("result_id", "");
    if (!reported.empty()) {
      id_map[reported] = record.id;
    }
    return {};
  }

  std::string proposed = call.value("result_id", "");
  if (proposed.empty()) {
    proposed = defect.value("id", "");
  }
  if (proposed.empty()) {
    proposed = next_defect_id(store, run_id);
  }
  // An agent snapshot can lag another milestone's defect; never overwrite a
  // different defect that happens to use the same id.
  auto clash = store.get_defect(proposed);
  while (clash.has_value() && clash->fingerprint != fingerprint) {
    proposed = next_defect_id(store, run_id);
    clash = store.get_defect(proposed);
  }

  store::DefectRecord record;
  record.id = proposed;
  record.run_id = std::string(run_id);
  record.milestone_id = milestone;
  record.status = defect.value("status", "OPEN");
  record.severity = severity;
  record.fingerprint = fingerprint;
  record.body_json = defect.dump();
  record.attempts = 0;
  auto stored = store.upsert_defect(record);
  if (!stored) {
    return stored;
  }
  const std::string reported = call.value("result_id", "");
  if (!reported.empty()) {
    id_map[reported] = record.id;
  }
  id_map[record.id] = record.id;
  return {};
}

Status apply_verify_defect(store::RunStore& store, const Json& call,
                           std::map<std::string, std::string>& id_map) {
  const Json arguments = call.value("arguments", Json::object());
  const std::string id = mapped_id(id_map, arguments.value("id", ""));
  const std::string status = arguments.value("result", "");
  if (id.empty() || (status != "VERIFIED" && status != "REOPENED")) {
    return fail("ORCH_INVALID_INTENT", "verify_defect with invalid arguments");
  }
  auto defect = store.get_defect(id);
  if (!defect) {
    // The agent verified a defect it filed in this turn but the intent order
    // was unexpected; nothing to apply.
    return {};
  }
  return store.set_defect_status(id, status, defect->attempts);
}

Status apply_dispute_test(store::RunStore& store, std::string_view run_id,
                          std::string_view milestone_id, const Json& call,
                          std::map<std::string, std::string>& id_map) {
  const Json arguments = call.value("arguments", Json::object());
  const std::string test_id = arguments.value("test_id", "");
  const std::string rationale = arguments.value("rationale", "");
  if (test_id.empty()) {
    return fail("ORCH_INVALID_INTENT", "dispute_test without a test id");
  }
  const std::string fingerprint = "test-dispute:" + test_id;
  auto existing = store.find_defect_by_fingerprint(run_id, fingerprint);
  std::string id;
  int disputes = 1;
  if (existing.has_value()) {
    id = existing->id;
    const Json previous = Json::parse(existing->body_json, nullptr, false);
    if (previous.is_object()) {
      disputes = previous.value("disputes", 1);
      if (previous.value("decided", false)) {
        ++disputes;
      }
    }
  } else {
    id = call.value("result_id", "");
    if (id.empty()) {
      id = next_defect_id(store, run_id);
    }
    auto clash = store.get_defect(id);
    while (clash.has_value() && clash->fingerprint != fingerprint) {
      id = next_defect_id(store, run_id);
      clash = store.get_defect(id);
    }
  }
  store::DefectRecord record;
  record.id = id;
  record.run_id = std::string(run_id);
  record.milestone_id = std::string(milestone_id);
  record.status = "DISPUTED";
  record.severity = "high";
  record.fingerprint = fingerprint;
  record.attempts = existing.has_value() ? existing->attempts : 0;
  record.body_json =
      Json{{"category", "test_dispute"},
           {"test_id", test_id},
           {"rationale", rationale},
           {"disputes", disputes},
           {"decided", false},
           {"summary", "test dispute: " + test_id}}
          .dump();
  auto stored = store.upsert_defect(record);
  if (!stored) {
    return stored;
  }
  const std::string reported = call.value("result_id", "");
  if (!reported.empty()) {
    id_map[reported] = record.id;
  }
  id_map[record.id] = record.id;
  return {};
}

Status apply_waiver_request(store::RunStore& store, std::string_view run_id,
                            std::string_view milestone_id, const Json& call,
                            std::uint32_t deadline_business_days) {
  const Json arguments = call.value("arguments", Json::object());
  store::WaiverRequestRecord request;
  request.run_id = std::string(run_id);
  request.milestone_id = std::string(milestone_id);
  request.rule = arguments.value("rule", "");
  request.scope = arguments.value("scope", "");
  request.justification = arguments.value("justification", "");
  request.requested_by = call.value("role", "agent");
  request.requested_at = store::unix_now();
  request.deadline_at =
      add_business_days(request.requested_at, deadline_business_days);
  auto recorded = store.record_waiver_request(request);
  if (!recorded) {
    return tl::make_unexpected(recorded.error());
  }
  return {};
}

}  // namespace

Status apply_agent_intents(store::RunStore& store, std::string_view run_id,
                          std::string_view milestone_id,
                          const std::vector<Json>& calls,
                          std::map<std::string, std::string>& id_map,
                          std::uint32_t waiver_deadline_business_days) {
  for (const auto& call : calls) {
    const std::string tool = call.value("tool", "");
    if (tool == "file_defect") {
      auto applied =
          apply_file_defect(store, run_id, milestone_id, call, id_map);
      if (!applied) {
        return applied;
      }
    } else if (tool == "verify_defect") {
      auto applied = apply_verify_defect(store, call, id_map);
      if (!applied) {
        return applied;
      }
    } else if (tool == "dispute_test") {
      auto applied =
          apply_dispute_test(store, run_id, milestone_id, call, id_map);
      if (!applied) {
        return applied;
      }
    } else if (tool == "request_waiver") {
      auto applied =
          apply_waiver_request(store, run_id, milestone_id, call,
                                 waiver_deadline_business_days);
      if (!applied) {
        return applied;
      }
    }
  }
  return {};
}

}  // namespace cf::orch
