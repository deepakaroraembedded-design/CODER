// cf-mcp: the MCP tool surface (design section 9.1).
//
// A role-filtered registry of tools with JSON-Schema-declared arguments.
// Arguments are validated before dispatch; every failure returns a structured
// error so the agent can correct itself. Tools whose subsystem is not built
// yet return TOOL_UNAVAILABLE instead of pretending to work.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/codeintel/symbol_index.hpp"
#include "cf/common/process.hpp"
#include "cf/common/result.hpp"
#include "cf/gates/profile.hpp"
#include "cf/rag/catalog.hpp"
#include "cf/rag/retriever.hpp"
#include "cf/store/run_store.hpp"

namespace cf::mcp {

using Json = nlohmann::json;

enum class ToolRole { Coder, Reviewer, Qa };

std::string_view to_string(ToolRole role);
std::optional<ToolRole> tool_role_from_string(std::string_view text);

struct ToolResult {
  bool is_error = false;
  std::string text;
  // Null when there is nothing structured to return; otherwise an object
  // (MCP structured content must be a record, never a bare array).
  Json structured;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  Json input_schema;
  std::vector<ToolRole> roles;
  std::function<ToolResult(const Json& arguments)> handler;
};

class ToolRegistry {
 public:
  void add(ToolDefinition definition);

  const ToolDefinition* find(std::string_view name) const;

  // MCP tools/list payload for one role.
  Json list_for_role(ToolRole role) const;

  // Checks the role, validates the arguments against the declared schema,
  // then dispatches.
  ToolResult call(ToolRole role, std::string_view name, const Json& arguments) const;

 private:
  std::map<std::string, ToolDefinition, std::less<>> tools_;
};

// Minimal JSON Schema checks for tool arguments: type, required, properties,
// additionalProperties: false and items. Returns an error message, or nullopt
// when the value conforms.
std::optional<std::string> validate_arguments(const Json& schema, const Json& arguments);

// The trusted gate profile and workspace an agent-side run_checks can use
// (design section 9.1: a dry run on the agent's worktree).
struct GateCheckContext {
  gates::Profile profile;
  std::string language;
  std::string build_system;
  std::string test_framework;
  std::string standard;
  std::string standard_var;
  std::filesystem::path worktree;
  std::filesystem::path build_dir;
  std::filesystem::path rules_dir;
  std::chrono::milliseconds step_timeout{300000};
  process::ProcessExecutor* executor = nullptr;
};

struct ToolDependencies {
  std::string run_id;
  std::string milestone_id;
  std::string role;  // coder | reviewer | qa (the server's role)
  rag::QueryContext context;  // product/component/language/as_of for RAG
  rag::Retriever* retriever = nullptr;
  // Default result count for search_guidelines when the call omits top_k
  // (design section 15, rag.top_k).
  std::uint32_t default_top_k = 8;
  store::RunStore* store = nullptr;
  // The curated pattern catalog (design section 6.2); get_pattern reads it
  // directly and falls back to the retrieval index when it is absent.
  const std::vector<rag::PatternEntry>* patterns = nullptr;
  // The code intelligence symbol index (design section 6.5); find_symbol and
  // get_callers read it. Null until a run indexes its code.
  const codeintel::SymbolIndex* symbols = nullptr;
  // Gate dry-run context; run_checks returns TOOL_UNAVAILABLE when null.
  const GateCheckContext* gate_checks = nullptr;
  // Pinned reference-document versions by library (design section 6.3.1);
  // lookup_reference uses it when the agent omits the version.
  const std::map<std::string, std::string, std::less<>>* reference_versions =
      nullptr;
  // Business-day deadline for a request_waiver request (design section
  // 12.1); defaults to two business days.
  std::uint32_t waiver_deadline_business_days = 2;
};

// Builds the design section 9.1 tool surface.
ToolRegistry build_tool_registry(ToolDependencies dependencies);

}  // namespace cf::mcp
