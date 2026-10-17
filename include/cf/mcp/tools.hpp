// cf-mcp: the MCP tool surface (design section 9.1).
//
// A role-filtered registry of tools with JSON-Schema-declared arguments.
// Arguments are validated before dispatch; every failure returns a structured
// error so the agent can correct itself. Tools whose subsystem is not built
// yet return TOOL_UNAVAILABLE instead of pretending to work.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
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

struct ToolDependencies {
  std::string run_id;
  std::string milestone_id;
  rag::QueryContext context;  // product/component/language/as_of for RAG
  rag::Retriever* retriever = nullptr;
  store::RunStore* store = nullptr;
  // The curated pattern catalog (design section 6.2); get_pattern reads it
  // directly and falls back to the retrieval index when it is absent.
  const std::vector<rag::PatternEntry>* patterns = nullptr;
};

// Builds the design section 9.1 tool surface.
ToolRegistry build_tool_registry(ToolDependencies dependencies);

}  // namespace cf::mcp
