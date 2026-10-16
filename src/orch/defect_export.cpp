#include "cf/orch/defect_export.hpp"

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/hash.hpp"
#include "cf/github/secrets.hpp"

namespace cf::orch {
namespace {

using Json = nlohmann::json;

// The state an issue should be in for a defect status (design section 6.10).
std::string desired_issue_state(const std::string& defect_status) {
  return defect_status == "VERIFIED" ? "closed" : "open";
}

bool is_terminal_for_issue(const std::string& defect_status) {
  return defect_status == "WONT_FIX";
}

}  // namespace

DefectExporter::DefectExporter(store::RunStore& store, Dispatcher& dispatcher,
                               DefectExportConfig config)
    : store_(store), dispatcher_(dispatcher), config_(std::move(config)) {}

std::string DefectExporter::build_body(std::string_view run_id,
                                       const store::DefectRecord& defect,
                                       bool redact) const {
  const Json body = Json::parse(defect.body_json, nullptr, false);
  const Json& fields = body.is_object() ? body : Json::object();

  std::string text = fields.value("summary", "defect " + defect.id) + "\n\n";
  text += "- milestone: " + defect.milestone_id + "\n";
  text += "- severity: " + defect.severity + "\n";
  text += "- status: " + defect.status + "\n";
  if (const auto requirements = fields.find("requirement_ids");
      requirements != fields.end() && requirements->is_array()) {
    std::string joined;
    for (const auto& requirement : *requirements) {
      if (!joined.empty()) {
        joined += ", ";
      }
      joined += requirement.get<std::string>();
    }
    text += "- requirements: " + joined + "\n";
  }
  if (const auto category = fields.find("category");
      category != fields.end() && category->is_string()) {
    text += "- category: " + category->get<std::string>() + "\n";
  }
  text += "- run report: " + config_.report_base + "/" + std::string(run_id) +
          "/report.json\n";

  const bool redacted =
      redact || (fields.contains("repro") &&
                 !github::scan_for_secrets(fields.value("repro", "")).empty()) ||
      (fields.contains("summary") &&
       !github::scan_for_secrets(fields.value("summary", "")).empty());
  if (!redacted) {
    const std::string repro = fields.value("repro", "");
    if (!repro.empty()) {
      text += "\nRepro:\n```\n" + repro + "\n```\n";
    }
    if (const auto evidence = fields.find("evidence");
        evidence != fields.end() && evidence->is_object()) {
      const std::string log_ref = evidence->value("log_ref", "");
      if (!log_ref.empty()) {
        text += "\nEvidence: " + log_ref + "\n";
      }
    }
  }
  if (redacted) {
    text += "\n(repro and evidence are omitted here: the run used restricted "
            "documents or the content matched a secret pattern; see the run "
            "report)\n";
  }
  return text;
}

Result<int64_t> DefectExporter::confirmed_issue_number(
    std::string_view run_id, std::string_view defect_id) const {
  auto operations = store_.list_operations(run_id, {store::op_state::kConfirmed});
  if (!operations) {
    return tl::make_unexpected(operations.error());
  }
  for (const auto& operation : *operations) {
    if (operation.kind != "issue_create") {
      continue;
    }
    const Json input = Json::parse(operation.input_json, nullptr, false);
    if (input.is_discarded() || input.value("defect_id", "") != defect_id) {
      continue;
    }
    const Json result = Json::parse(operation.result_json, nullptr, false);
    if (!result.is_discarded()) {
      const int64_t number = result.value("number", int64_t{0});
      if (number != 0) {
        return number;
      }
    }
  }
  return int64_t{0};
}

Status DefectExporter::ensure_issue(std::string_view run_id,
                                    const store::DefectRecord& defect, int64_t number,
                                    store::UnixTime now) {
  if (number != 0) {
    return {};
  }
  const std::string key = cf::idempotency_key(run_id, defect.milestone_id,
                                              "issue_create", cf::sha256_hex(defect.id));
  if (store_.find_operation_by_key(key).has_value()) {
    return {};
  }
  const Json fields = Json::parse(defect.body_json, nullptr, false);
  const Json& body = fields.is_object() ? fields : Json::object();
  const std::string summary =
      body.value("summary", "defect " + defect.id);
  const std::string body_text = build_body(run_id, defect, config_.restricted);
  std::vector<std::string> labels = {"cf-defect",
                                     "severity:" + defect.severity,
                                     "milestone:" + defect.milestone_id};
  if (const auto requirements = body.find("requirement_ids");
      requirements != body.end() && requirements->is_array()) {
    for (const auto& requirement : *requirements) {
      labels.push_back("requirement:" + requirement.get<std::string>());
    }
  }

  store::NewOperation intent;
  intent.idempotency_key = key;
  intent.milestone_id = defect.milestone_id;
  intent.kind = "issue_create";
  intent.input_json = Json{{"title", "[" + defect.id + "] " + summary},
                           {"body", body_text},
                           {"labels", labels},
                           {"defect_id", defect.id},
                           {"fingerprint", defect.fingerprint}}
                          .dump();
  intent.expected_json = "{}";
  const auto recorded = store_.record_intents(run_id, {intent}, now);
  if (!recorded && recorded.error().code != store::error_codes::kConflict) {
    return recorded;
  }
  return {};
}

Status DefectExporter::sync_status(std::string_view run_id,
                                   const store::DefectRecord& defect, int64_t number,
                                   store::UnixTime now) {
  if (number == 0 || is_terminal_for_issue(defect.status)) {
    return {};
  }
  const std::string state = desired_issue_state(defect.status);
  const std::string body = build_body(run_id, defect, config_.restricted);

  // What the confirmed operations already put on the issue: the create body
  // (state open) or the newest update.
  auto operations = store_.list_operations(run_id, {store::op_state::kConfirmed});
  if (!operations) {
    return tl::make_unexpected(operations.error());
  }
  std::string last_body;
  std::string last_state;
  store::UnixTime last_at = 0;
  for (const auto& operation : *operations) {
    const Json input = Json::parse(operation.input_json, nullptr, false);
    if (input.is_discarded()) {
      continue;
    }
    if (operation.kind == "issue_create") {
      if (input.value("defect_id", "") != defect.id || operation.updated_at < last_at) {
        continue;
      }
      last_at = operation.updated_at;
      last_body = input.value("body", "");
      last_state = "open";
    } else if (operation.kind == "issue_update") {
      if (input.value("number", int64_t{0}) != number || operation.updated_at < last_at) {
        continue;
      }
      last_at = operation.updated_at;
      last_body = input.value("body", "");
      last_state = input.value("state", "");
    }
  }
  if (last_body == body && last_state == state) {
    return {};  // the issue already shows the intended state
  }

  const std::string key = cf::idempotency_key(
      run_id, defect.milestone_id, "issue_update",
      cf::sha256_hex(defect.id + "|" + body + "|" + state));
  if (store_.find_operation_by_key(key).has_value()) {
    return {};
  }
  store::NewOperation intent;
  intent.idempotency_key = key;
  intent.milestone_id = defect.milestone_id;
  intent.kind = "issue_update";
  intent.input_json =
      Json{{"number", number}, {"body", body}, {"state", state}, {"defect_id", defect.id}}
          .dump();
  intent.expected_json = "{}";
  const auto recorded = store_.record_intents(run_id, {intent}, now);
  if (!recorded && recorded.error().code != store::error_codes::kConflict) {
    return recorded;
  }
  return {};
}

Result<std::size_t> DefectExporter::sync(std::string_view run_id, uint64_t epoch,
                                         store::UnixTime now) {
  auto defects = store_.list_defects(run_id);
  if (!defects) {
    return tl::make_unexpected(defects.error());
  }
  for (const auto& stored : *defects) {
    store::DefectRecord defect = stored;
    auto number = confirmed_issue_number(run_id, defect.id);
    if (!number) {
      return tl::make_unexpected(number.error());
    }

    // A human closing the issue as "not planned" becomes WONT_FIX; run it
    // before the status sync so the issue is not reopened. Best effort: a
    // transient read failure must not stop the run.
    if (*number != 0 && config_.client != nullptr &&
        defect.status != "VERIFIED" && defect.status != "WONT_FIX") {
      auto issue = config_.client->get_issue(config_.owner, config_.repo, *number);
      if (issue.has_value() && issue->state == "closed") {
        const auto stored_status =
            store_.set_defect_status(defect.id, "WONT_FIX", defect.attempts);
        if (!stored_status) {
          return tl::make_unexpected(stored_status.error());
        }
        defect.status = "WONT_FIX";
      }
    }

    auto ensured = ensure_issue(run_id, defect, *number, now);
    if (!ensured) {
      return tl::make_unexpected(ensured.error());
    }
    auto synced = sync_status(run_id, defect, *number, now);
    if (!synced) {
      return tl::make_unexpected(synced.error());
    }
  }
  return dispatcher_.dispatch_pending(epoch, now);
}

}  // namespace cf::orch
