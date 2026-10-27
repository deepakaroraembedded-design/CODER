#include "cf/mcp/tools.hpp"

#include <algorithm>
#include <utility>

#include "cf/codeintel/context_packer.hpp"
#include "cf/common/hash.hpp"

namespace cf::mcp {
namespace {

ToolResult error_result(std::string code, std::string message) {
  ToolResult result;
  result.is_error = true;
  result.text = message;
  result.structured = Json{{"code", std::move(code)}, {"message", std::move(message)}};
  return result;
}

ToolResult unavailable(const std::string& tool) {
  return error_result("TOOL_UNAVAILABLE",
                      tool + " is not available in this build: its subsystem "
                             "(cf-codeintel, gates or sandbox) is not implemented yet");
}

Json string_property(const std::string& description) {
  return Json{{"type", "string"}, {"description", description}};
}

Json integer_property(const std::string& description) {
  return Json{{"type", "integer"}, {"description", description}};
}

Json object_schema(Json properties, std::vector<std::string> required) {
  return Json{{"type", "object"},
              {"properties", std::move(properties)},
              {"required", std::move(required)},
              {"additionalProperties", false}};
}

Json hits_json(const std::vector<rag::Hit>& hits) {
  Json array = Json::array();
  for (const auto& hit : hits) {
    array.push_back(Json{{"chunk_id", hit.chunk_id},
                         {"doc_id", hit.doc_id},
                         {"version", hit.version},
                         {"type", std::string(rag::to_string(hit.type))},
                         {"text", hit.text},
                         {"citation", hit.citation},
                         {"score", hit.score},
                         {"conflict", hit.conflict},
                         {"overridden_by", hit.overridden_by},
                         {"deciding_rule", hit.deciding_rule},
                         {"unresolved_conflict", hit.unresolved_conflict}});
  }
  return array;
}

Result<std::vector<rag::Hit>> search_rag(const ToolDependencies& dependencies,
                                         std::string text,
                                         std::vector<rag::DocType> types,
                                         std::uint32_t top_k) {
  if (dependencies.retriever == nullptr) {
    return fail("TOOL_UNAVAILABLE", "no retrieval index is configured");
  }
  rag::Query query;
  query.text = std::move(text);
  query.types = std::move(types);
  query.top_k = top_k;
  query.context = dependencies.context;
  return dependencies.retriever->search(query);
}

// The text content is what the agent model actually sees (opencode surfaces
// only the text part of an MCP tool result), so the hits are rendered here in
// full; structuredContent carries the same data for programmatic clients.
std::string hits_text(const std::vector<rag::Hit>& hits) {
  std::string out;
  for (std::size_t i = 0; i < hits.size(); ++i) {
    const rag::Hit& hit = hits[i];
    if (i > 0) {
      out += "\n";
    }
    out += "[" + std::to_string(i + 1) + "] " + hit.doc_id + "@" + hit.version + " (" +
           std::string(rag::to_string(hit.type)) + ")";
    if (!hit.citation.empty()) {
      out += " citation: " + hit.citation;
    }
    out += "\n";
    if (hit.unresolved_conflict) {
      out += "warning: unresolved conflict";
      out += "\n";
    } else if (!hit.overridden_by.empty()) {
      out += "note: overrides";
      for (std::size_t j = 0; j < hit.overridden_by.size(); ++j) {
        out += j == 0 ? " " : ", ";
        out += hit.overridden_by[j];
      }
      if (!hit.deciding_rule.empty()) {
        out += " (" + hit.deciding_rule + ")";
      }
      out += "\n";
    }
    out += hit.text;
    out += "\n";
  }
  return out;
}

ToolResult hits_result(const std::vector<rag::Hit>& hits, const std::string& empty_message) {
  ToolResult result;
  // MCP structured content must be an object, never a bare array.
  result.structured = Json{{"hits", hits_json(hits)}};
  result.text = hits.empty() ? empty_message : hits_text(hits);
  return result;
}

}  // namespace

std::string_view to_string(ToolRole role) {
  switch (role) {
    case ToolRole::Coder: return "coder";
    case ToolRole::Reviewer: return "reviewer";
    case ToolRole::Qa: return "qa";
  }
  return "unknown";
}

std::optional<ToolRole> tool_role_from_string(std::string_view text) {
  if (text == "coder") {
    return ToolRole::Coder;
  }
  if (text == "reviewer") {
    return ToolRole::Reviewer;
  }
  if (text == "qa") {
    return ToolRole::Qa;
  }
  return std::nullopt;
}

void ToolRegistry::add(ToolDefinition definition) {
  const std::string name = definition.name;
  tools_[name] = std::move(definition);
}

const ToolDefinition* ToolRegistry::find(std::string_view name) const {
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second;
}

Json ToolRegistry::list_for_role(ToolRole role) const {
  Json tools = Json::array();
  for (const auto& [name, tool] : tools_) {
    (void)name;
    if (std::find(tool.roles.begin(), tool.roles.end(), role) == tool.roles.end()) {
      continue;
    }
    tools.push_back(Json{{"name", tool.name},
                         {"description", tool.description},
                         {"inputSchema", tool.input_schema}});
  }
  return Json{{"tools", std::move(tools)}};
}

ToolResult ToolRegistry::call(ToolRole role, std::string_view name,
                              const Json& arguments) const {
  const auto it = tools_.find(name);
  if (it == tools_.end()) {
    return error_result("TOOL_NOT_FOUND", "unknown tool: " + std::string(name));
  }
  const ToolDefinition& tool = it->second;
  if (std::find(tool.roles.begin(), tool.roles.end(), role) == tool.roles.end()) {
    return error_result("TOOL_DENIED",
                        "tool '" + std::string(name) + "' is not available to the " +
                            std::string(to_string(role)) + " role");
  }
  if (const auto error = validate_arguments(tool.input_schema, arguments)) {
    return error_result("TOOL_INVALID_ARGUMENTS", *error);
  }
  return tool.handler(arguments);
}

namespace {

std::optional<std::string> validate_value(const Json& schema, const Json& value,
                                          const std::string& path) {
  if (!schema.is_object()) {
    return std::nullopt;
  }
  const auto type = schema.find("type");
  if (type != schema.end() && type->is_string()) {
    const std::string& expected = type->get_ref<const std::string&>();
    bool ok = true;
    if (expected == "object") {
      ok = value.is_object();
    } else if (expected == "array") {
      ok = value.is_array();
    } else if (expected == "string") {
      ok = value.is_string();
    } else if (expected == "integer") {
      ok = value.is_number_integer();
    } else if (expected == "number") {
      ok = value.is_number();
    } else if (expected == "boolean") {
      ok = value.is_boolean();
    }
    if (!ok) {
      return path + " must be of type " + expected;
    }
  }
  if (value.is_object()) {
    const auto required = schema.find("required");
    if (required != schema.end() && required->is_array()) {
      for (const auto& key : *required) {
        if (!key.is_string()) {
          continue;
        }
        if (!value.contains(key.get<std::string>())) {
          return path + " is missing required field '" + key.get<std::string>() + "'";
        }
      }
    }
    const auto properties = schema.find("properties");
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (properties != schema.end() && properties->is_object()) {
        const auto property = properties->find(it.key());
        if (property != properties->end()) {
          if (const auto error = validate_value(*property, it.value(),
                                                path + "." + it.key())) {
            return error;
          }
          continue;
        }
      }
      const auto additional = schema.find("additionalProperties");
      if (additional != schema.end() && additional->is_boolean() &&
          !additional->get<bool>()) {
        return path + " has unexpected field '" + it.key() + "'";
      }
    }
  }
  if (value.is_array()) {
    const auto items = schema.find("items");
    if (items != schema.end()) {
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (const auto error =
                validate_value(*items, value[i], path + "[" + std::to_string(i) + "]")) {
          return error;
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> validate_arguments(const Json& schema,
                                              const Json& arguments) {
  return validate_value(schema, arguments, "arguments");
}

ToolRegistry build_tool_registry(ToolDependencies dependencies) {
  ToolRegistry registry;
  const std::vector<ToolRole> all = {ToolRole::Coder, ToolRole::Reviewer, ToolRole::Qa};

  registry.add(ToolDefinition{
      "search_guidelines",
      "Hybrid search over company documents with precedence and citations. "
      "Chunks with conflict=true were resolved by precedence; unresolved_conflict=true "
      "means the run must escalate instead of choosing.",
      object_schema(Json{{"query", string_property("The search text")},
                         {"types", Json{{"type", "array"},
                                        {"items", Json{{"type", "string"}}},
                                        {"description", "Document types to include "
                                                        "(policy, requirements, "
                                                        "architecture, design, guideline, "
                                                        "pattern, external_reference)"}}},
                         {"top_k", integer_property("Maximum number of results")}},
                    {"query"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        std::vector<rag::DocType> types;
        const auto types_it = arguments.find("types");
        if (types_it != arguments.end() && types_it->is_array()) {
          for (const auto& entry : *types_it) {
            const auto type = rag::doc_type_from_string(entry.get<std::string>());
            if (!type) {
              return error_result("TOOL_INVALID_ARGUMENTS",
                                  "unknown document type '" +
                                      entry.get<std::string>() + "'");
            }
            types.push_back(*type);
          }
        }
        auto hits = search_rag(
            dependencies, arguments.at("query").get<std::string>(),
            std::move(types),
            static_cast<std::uint32_t>(
                arguments.value("top_k", dependencies.default_top_k)));
        if (!hits) {
          return error_result(hits.error().code, hits.error().message);
        }
        return hits_result(*hits, "no matching guidance");
      }});

  registry.add(ToolDefinition{
      "get_document",
      "Returns the full text of a document, optionally a specific version. "
      "Without a version the newest ingested version is returned.",
      object_schema(Json{{"doc_id", string_property("Document id")},
                         {"version", string_property("Document version")}},
                    {"doc_id"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.retriever == nullptr) {
          return unavailable("get_document");
        }
        const std::string doc_id = arguments.at("doc_id").get<std::string>();
        const std::string version = arguments.value("version", "");
        auto document = dependencies.retriever->get(doc_id, version);
        if (!document && version.empty()) {
          // Models copy the "doc_id@version" form rendered in search hits;
          // accept it as a convenience rather than failing the lookup.
          const auto at = doc_id.rfind('@');
          if (at != std::string::npos && at > 0 && at + 1 < doc_id.size()) {
            document = dependencies.retriever->get(doc_id.substr(0, at),
                                                   doc_id.substr(at + 1));
          }
        }
        if (!document) {
          return error_result(document.error().code == rag::error_codes::kNotFound
                                  ? "TOOL_NOT_FOUND"
                                  : document.error().code,
                              document.error().message);
        }
        ToolResult result;
        result.structured = Json{{"doc_id", document->doc_id},
                                 {"version", document->version},
                                 {"type", std::string(rag::to_string(document->type))},
                                 {"owner", document->owner},
                                 {"classification", document->classification},
                                 {"scope", document->scope},
                                 {"status", document->status},
                                 {"license", document->license},
                                 {"text", document->text}};
        result.text = document->text;
        return result;
      }});

  registry.add(ToolDefinition{
      "get_requirement",
      "Returns the requirement text and its acceptance criteria for a requirement id.",
      object_schema(Json{{"req_id", string_property("Requirement id, e.g. REQ-010")}},
                    {"req_id"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        auto hits = search_rag(dependencies, arguments.at("req_id").get<std::string>(),
                               {rag::DocType::Requirements}, 1);
        if (!hits) {
          return error_result(hits.error().code, hits.error().message);
        }
        if (hits->empty()) {
          return error_result("TOOL_NOT_FOUND",
                              "requirement not found: " +
                                  arguments.at("req_id").get<std::string>());
        }
        return hits_result(*hits, "no requirement found");
      }});

  registry.add(ToolDefinition{
      "get_design",
      "Returns the ADRs, interface contracts and chosen patterns for a component.",
      object_schema(Json{{"component", string_property("Component name")}}, {"component"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        auto hits = search_rag(dependencies, arguments.at("component").get<std::string>(),
                               {rag::DocType::Design, rag::DocType::Architecture}, 5);
        if (!hits) {
          return error_result(hits.error().code, hits.error().message);
        }
        return hits_result(*hits, "no design recorded for this component");
      }});

  registry.add(ToolDefinition{
      "get_pattern",
      "Returns a pattern catalog entry with its reference implementation and checks.",
      object_schema(Json{{"pattern_id", string_property("Pattern id, e.g. PAT-STRATEGY")}},
                    {"pattern_id"}),
      {ToolRole::Coder, ToolRole::Reviewer},
      [dependencies](const Json& arguments) -> ToolResult {
        const std::string pattern_id = arguments.at("pattern_id").get<std::string>();
        // The curated catalog is authoritative (design section 6.2); the
        // retrieval index is the fallback when no catalog is configured.
        if (dependencies.patterns != nullptr) {
          for (const auto& entry : *dependencies.patterns) {
            if (entry.id != pattern_id) {
              continue;
            }
            ToolResult result;
            result.text = entry.to_markdown();
            result.structured = Json{
                {"id", entry.id},
                {"name", entry.name},
                {"level", entry.level},
                {"use_when", entry.use_when},
                {"avoid_when", entry.avoid_when},
                {"company_notes", entry.company_notes},
                {"reference_impl", entry.reference_impl},
                {"conformance_checks", entry.conformance_checks},
                {"source", entry.source_path}};
            return result;
          }
        }
        auto hits = search_rag(dependencies, pattern_id, {rag::DocType::Pattern}, 1);
        if (!hits) {
          return error_result(hits.error().code, hits.error().message);
        }
        if (hits->empty()) {
          return error_result("TOOL_NOT_FOUND", "pattern not found: " + pattern_id);
        }
        return hits_result(*hits, "no pattern found");
      }});

  registry.add(ToolDefinition{
      "lookup_reference",
      "Searches the offline reference documentation mirror. Returns covered=false "
      "when the mirror does not have the requested library or version; the agent must "
      "then record a gap instead of guessing.",
      object_schema(Json{{"query", string_property("What to look up")},
                         {"library", string_property("Library name")},
                         {"version", string_property("Library version")}},
                    {"query"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        std::string query = arguments.at("query").get<std::string>();
        const std::string library = arguments.value("library", "");
        if (!library.empty()) {
          query += " " + library;
        }
        auto hits = search_rag(dependencies, query, {rag::DocType::ExternalReference}, 5);
        if (!hits) {
          return error_result(hits.error().code, hits.error().message);
        }
        const std::string version = arguments.value("version", "");
        if (!version.empty()) {
          hits->erase(std::remove_if(hits->begin(), hits->end(),
                                     [&version](const rag::Hit& hit) {
                                       return hit.version != version;
                                     }),
                      hits->end());
        }
        if (hits->empty()) {
          ToolResult result;
          result.text = "not covered by the offline reference mirror";
          result.structured = Json{{"covered", false}, {"hits", Json::array()}};
          return result;
        }
        ToolResult result;
        result.text = hits_text(*hits);
        result.structured = Json{{"covered", true}, {"hits", hits_json(*hits)}};
        return result;
      }});

  registry.add(ToolDefinition{
      "get_gate_results",
      "Returns the structured gate results, for a commit or the newest run.",
      object_schema(Json{{"commit", string_property("Commit SHA")}}, {}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.store == nullptr) {
          return unavailable("get_gate_results");
        }
        const std::string commit = arguments.value("commit", "");
        auto results =
            commit.empty()
                ? dependencies.store->latest_gate_results(dependencies.run_id,
                                                           dependencies.milestone_id)
                : dependencies.store->list_gate_results(dependencies.run_id, commit);
        if (!results) {
          return error_result(results.error().code == store::error_codes::kNotFound
                                  ? "TOOL_NOT_FOUND"
                                  : results.error().code,
                              results.error().message);
        }
        if (results->empty()) {
          return error_result("TOOL_NOT_FOUND",
                              "no gate results for commit " + commit);
        }
        Json array = Json::array();
        for (const auto& row : *results) {
          array.push_back(Json{{"gate", row.gate},
                               {"passed", row.passed},
                               {"milestone_id", row.milestone_id},
                               {"commit_sha", row.commit_sha},
                               {"report_ref", row.report_ref},
                               {"at", row.at}});
        }
        ToolResult result;
        result.structured = Json{{"gates", std::move(array)}};
        result.text = result.structured.dump(2);
        return result;
      }});

  registry.add(ToolDefinition{
      "list_defects",
      "Lists the defect queue, optionally by milestone and status.",
      object_schema(Json{{"milestone", string_property("Milestone id")},
                         {"status", Json{{"type", "array"},
                                         {"items", Json{{"type", "string"}}},
                                         {"description", "Statuses to include"}}}},
                    {}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.store == nullptr) {
          return unavailable("list_defects");
        }
        std::string milestone = arguments.value("milestone", "");
        if (milestone.empty()) {
          milestone = dependencies.milestone_id;
        }
        std::vector<std::string> statuses;
        const auto status_it = arguments.find("status");
        if (status_it != arguments.end() && status_it->is_array()) {
          for (const auto& entry : *status_it) {
            statuses.push_back(entry.get<std::string>());
          }
        }
        auto defects =
            dependencies.store->list_defects(dependencies.run_id, milestone, statuses);
        if (!defects) {
          return error_result(defects.error().code, defects.error().message);
        }
        Json array = Json::array();
        for (const auto& defect : *defects) {
          array.push_back(Json{{"id", defect.id},
                               {"milestone_id", defect.milestone_id},
                               {"status", defect.status},
                               {"severity", defect.severity},
                               {"fingerprint", defect.fingerprint},
                               {"attempts", defect.attempts},
                               {"body", Json::parse(defect.body_json, nullptr, false)}});
        }
        ToolResult result;
        result.structured = Json{{"defects", std::move(array)}};
        result.text = result.structured.dump(2);
        return result;
      }});

  registry.add(ToolDefinition{
      "file_defect",
      "Files a defect, de-duplicated by fingerprint. A recurring defect updates the "
      "existing record and is marked REOPENED.",
      object_schema(
          Json{{"defect_json",
                Json{{"type", "object"},
                     {"description", "The defect payload (design section 8.3)"},
                     {"required", Json::array({"summary", "severity"})},
                     {"properties",
                      Json{{"id", string_property("Defect id (generated when omitted)")},
                           {"milestone_id", string_property("Milestone id")},
                           {"requirement_ids", Json{{"type", "array"},
                                                    {"items", Json{{"type", "string"}}}}},
                           {"test_id", string_property("Failing test id")},
                           {"severity", string_property("blocker|major|minor|high|medium|low")},
                           {"category", string_property("Defect category")},
                           {"summary", string_property("One-line summary")},
                           {"repro", string_property("Reproduction command")},
                           {"evidence", Json{{"type", "object"}}},
                           {"fingerprint", string_property("Stable fingerprint")},
                           {"status", string_property("Initial status")}}}}}},
          {"defect_json"}),
      {ToolRole::Qa},
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.store == nullptr) {
          return unavailable("file_defect");
        }
        const Json& defect = arguments.at("defect_json");
        std::string fingerprint = defect.value("fingerprint", "");
        if (fingerprint.empty()) {
          fingerprint = cf::sha256_hex(defect.value("test_id", "") + "\n" +
                                       defect.value("summary", ""));
        }
        const std::string milestone =
            defect.value("milestone_id", dependencies.milestone_id);
        const std::string severity = defect.value("severity", "medium");

        auto existing =
            dependencies.store->find_defect_by_fingerprint(dependencies.run_id, fingerprint);
        std::string id;
        bool deduplicated = false;
        if (existing.has_value()) {
          id = existing->id;
          deduplicated = true;
          store::DefectRecord record = *existing;
          record.milestone_id = milestone;
          record.severity = severity;
          record.body_json = defect.dump();
          record.status = "REOPENED";
          const auto stored = dependencies.store->upsert_defect(record);
          if (!stored) {
            return error_result(stored.error().code, stored.error().message);
          }
        } else {
          auto all = dependencies.store->list_defects(dependencies.run_id);
          const std::size_t count = all.has_value() ? all->size() : 0;
          id = defect.value("id", "D-" + std::to_string(count + 1));
          store::DefectRecord record;
          record.id = id;
          record.run_id = dependencies.run_id;
          record.milestone_id = milestone;
          record.status = defect.value("status", "OPEN");
          record.severity = severity;
          record.fingerprint = fingerprint;
          record.body_json = defect.dump();
          record.attempts = 0;
          const auto stored = dependencies.store->upsert_defect(record);
          if (!stored) {
            return error_result(stored.error().code, stored.error().message);
          }
        }
        ToolResult result;
        result.text = (deduplicated ? "updated defect " : "filed defect ") + id;
        result.structured = Json{
            {"id", id}, {"fingerprint", fingerprint}, {"deduplicated", deduplicated}};
        return result;
      }});

  registry.add(ToolDefinition{
      "verify_defect",
      "Marks a defect VERIFIED or REOPENED after re-testing.",
      object_schema(Json{{"id", string_property("Defect id")},
                         {"result", Json{{"type", "string"},
                                         {"description", "VERIFIED or REOPENED"}}}},
                    {"id", "result"}),
      {ToolRole::Qa},
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.store == nullptr) {
          return unavailable("verify_defect");
        }
        const std::string id = arguments.at("id").get<std::string>();
        const std::string result_status = arguments.at("result").get<std::string>();
        if (result_status != "VERIFIED" && result_status != "REOPENED") {
          return error_result("TOOL_INVALID_ARGUMENTS",
                              "result must be VERIFIED or REOPENED");
        }
        auto defect = dependencies.store->get_defect(id);
        if (!defect) {
          return error_result("TOOL_NOT_FOUND", defect.error().message);
        }
        const auto stored = dependencies.store->set_defect_status(
            id, result_status, defect->attempts);
        if (!stored) {
          return error_result(stored.error().code, stored.error().message);
        }
        ToolResult result;
        result.text = "defect " + id + " marked " + result_status;
        result.structured = Json{{"id", id}, {"status", result_status}};
        return result;
      }});

  registry.add(ToolDefinition{
      "submit_work",
      "Ends the coder turn and hands the work back to the orchestrator.",
      object_schema(Json{{"summary", string_property("What was changed and why")},
                         {"defect_ids", Json{{"type", "array"},
                                             {"items", Json{{"type", "string"}}},
                                             {"description", "Defects addressed"}}}},
                    {"summary", "defect_ids"}),
      {ToolRole::Coder},
      [](const Json& arguments) -> ToolResult {
        ToolResult result;
        result.text = "submit_work received: " + arguments.value("summary", "");
        result.structured = Json{{"accepted", true}};
        return result;
      }});

  registry.add(ToolDefinition{
      "dispute_test",
      "Requests a change to a protected test; never changes the test directly. "
      "The dispute is recorded as a DISPUTED defect and QA decides: a fix to "
      "the test, or a rejection. Disputing the same test again after a "
      "rejection escalates the milestone (design sections 6.9, 12).",
      object_schema(Json{{"test_id", string_property("Test id or path")},
                         {"rationale", string_property("Why the test should change")}},
                    {"test_id", "rationale"}),
      {ToolRole::Coder},
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.store == nullptr) {
          return unavailable("dispute_test");
        }
        const std::string test_id = arguments.at("test_id").get<std::string>();
        const std::string rationale = arguments.at("rationale").get<std::string>();
        const std::string fingerprint = "test-dispute:" + test_id;

        auto existing = dependencies.store->find_defect_by_fingerprint(
            dependencies.run_id, fingerprint);
        std::string id;
        int disputes = 1;
        bool repeated = false;
        if (existing.has_value()) {
          id = existing->id;
          const Json previous =
              Json::parse(existing->body_json, nullptr, false);
          if (previous.is_object()) {
            disputes = previous.value("disputes", 1);
            // Only a dispute raised after a QA decision counts as a repeat
            // (design section 12: QA rejects, then the coder disputes again).
            if (previous.value("decided", false)) {
              repeated = true;
              ++disputes;
            }
          }
        } else {
          auto all = dependencies.store->list_defects(dependencies.run_id);
          const std::size_t count = all.has_value() ? all->size() : 0;
          id = "D-" + std::to_string(count + 1);
        }
        store::DefectRecord record;
        record.id = id;
        record.run_id = dependencies.run_id;
        record.milestone_id = dependencies.milestone_id;
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
        const auto stored = dependencies.store->upsert_defect(record);
        if (!stored) {
          return error_result(stored.error().code, stored.error().message);
        }
        ToolResult result;
        result.text = "dispute recorded against " + test_id + " as defect " + id +
                      " (QA decides; never edit the test yourself)";
        result.structured = Json{{"id", id},
                                 {"status", "DISPUTED"},
                                 {"test_id", test_id},
                                 {"disputes", disputes},
                                 {"repeated", repeated}};
        return result;
      }});

  registry.add(ToolDefinition{
      "submit_review",
      "Ends the reviewer turn with a structured verdict (design section 8.2).",
      object_schema(Json{{"verdict_json", Json{{"type", "object"},
                                               {"description", "The review verdict"}}}},
                    {"verdict_json"}),
      {ToolRole::Reviewer},
      [](const Json& arguments) -> ToolResult {
        (void)arguments;
        ToolResult result;
        result.text = "submit_review received";
        result.structured = Json{{"accepted", true}};
        return result;
      }});

  registry.add(ToolDefinition{
      "submit_tests",
      "Ends the QA test-writing turn.",
      object_schema(Json{{"plan_ref", string_property("Reference to the test plan")},
                         {"summary", string_property("What was written")}},
                    {"plan_ref", "summary"}),
      {ToolRole::Qa},
      [](const Json& arguments) -> ToolResult {
        ToolResult result;
        result.text = "submit_tests received: " + arguments.value("summary", "");
        result.structured = Json{{"accepted", true}};
        return result;
      }});

  registry.add(ToolDefinition{
      "get_context",
      "Packed code context within a token budget (cf-codeintel, design "
      "section 6.5). task_id names what the task touches: a symbol name, or "
      "an indexed file path or component directory.",
      object_schema(Json{{"task_id", string_property(
                                 "Symbol name, file path or component directory")},
                         {"token_budget", integer_property("Token budget")}},
                    {"task_id", "token_budget"}),
      {ToolRole::Coder, ToolRole::Reviewer},
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.symbols == nullptr) {
          return unavailable("get_context");
        }
        codeintel::ContextPacker packer(*dependencies.symbols);
        codeintel::ContextRequest request;
        request.seeds = {arguments["task_id"].get<std::string>()};
        const std::int64_t budget = arguments["token_budget"].get<int64_t>();
        request.token_budget =
            budget > 0 ? static_cast<std::uint32_t>(budget) : 0;
        auto packed = packer.pack(request);
        if (!packed) {
          return error_result(packed.error().code, packed.error().message);
        }
        if (packed->included.empty() && packed->dropped.empty()) {
          return error_result(
              "TOOL_NOT_FOUND",
              "no symbol or file matching '" +
                  arguments["task_id"].get<std::string>() + "' in the symbol index");
        }
        ToolResult result;
        result.structured =
            Json{{"context", packed->text},
                 {"tokens", packed->tokens},
                 {"included", packed->included},
                 {"dropped", packed->dropped},
                 {"unresolved", packed->unresolved}};
        std::string text = packed->text;
        if (!packed->dropped.empty()) {
          text += "\n(budget cut " + std::to_string(packed->dropped.size()) +
                  " entrie(s))";
        }
        result.text = std::move(text);
        return result;
      }});

  registry.add(ToolDefinition{
      "find_symbol",
      "Finds a symbol definition in the symbol index (cf-codeintel).",
      object_schema(Json{{"name", string_property("Symbol name")}}, {"name"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.symbols == nullptr) {
          return unavailable("find_symbol");
        }
        auto symbols =
            dependencies.symbols->find_symbol(arguments["name"].get<std::string>());
        if (!symbols) {
          return error_result(symbols.error().code, symbols.error().message);
        }
        Json array = Json::array();
        std::string text;
        for (const auto& symbol : *symbols) {
          array.push_back(Json{
              {"name", symbol.name},
              {"qualified", symbol.qualified},
              {"kind", std::string(codeintel::to_string(symbol.kind))},
              {"file", symbol.file},
              {"line", symbol.line},
              {"signature", symbol.signature},
              {"doc", symbol.doc},
              {"is_definition", symbol.is_definition}});
          if (!text.empty()) {
            text += "\n";
          }
          text += symbol.name + " (" + std::string(codeintel::to_string(symbol.kind)) +
                  ") — " + symbol.file + ":" + std::to_string(symbol.line) + "\n  " +
                  symbol.signature;
        }
        ToolResult result;
        result.structured = Json{{"symbols", std::move(array)}};
        result.text = symbols->empty()
                          ? "no symbol named '" + arguments["name"].get<std::string>() + "'"
                          : std::move(text);
        return result;
      }});

  registry.add(ToolDefinition{
      "get_callers",
      "Lists the callers of a symbol (cf-codeintel).",
      object_schema(Json{{"symbol", string_property("Symbol name")}}, {"symbol"}),
      all,
      [dependencies](const Json& arguments) -> ToolResult {
        if (dependencies.symbols == nullptr) {
          return unavailable("get_callers");
        }
        auto callers =
            dependencies.symbols->get_callers(arguments["symbol"].get<std::string>());
        if (!callers) {
          return error_result(callers.error().code, callers.error().message);
        }
        Json array = Json::array();
        std::string text;
        for (const auto& edge : *callers) {
          array.push_back(Json{{"caller", edge.caller},
                               {"callee", edge.callee},
                               {"file", edge.file},
                               {"line", edge.line}});
          if (!text.empty()) {
            text += "\n";
          }
          text += edge.caller + " — " + edge.file + ":" + std::to_string(edge.line);
        }
        ToolResult result;
        result.structured = Json{{"callers", std::move(array)}};
        result.text =
            callers->empty()
                ? "no recorded callers of '" + arguments["symbol"].get<std::string>() + "'"
                : std::move(text);
        return result;
      }});

  registry.add(ToolDefinition{
      "run_checks",
      "Runs a subset of gates on the agent's worktree (dry run).",
      object_schema(Json{{"scope", string_property("Files or component to check")}}, {}),
      {ToolRole::Coder, ToolRole::Qa},
      [](const Json& arguments) -> ToolResult {
        (void)arguments;
        return unavailable("run_checks");
      }});

  return registry;
}

}  // namespace cf::mcp
