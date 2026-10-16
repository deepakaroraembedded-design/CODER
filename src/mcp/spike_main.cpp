// cf-mcp spike binary (Phase 0a).
//
// Implements the MCP stdio tool surface the harness needs:
//   submit_work        - the structured coder submission (scenario H2)
//   spike_echo         - schema-validated echo (H6, H12)
//   spike_error        - tool error result (H5)
//   spike_timeout      - blocks past the client timeout (H5)
//   spike_invalid_json - malformed JSON-RPC output (H5)
//
// Every tools/call is appended as one JSON line to CF_MCP_CALLS_FILE so the
// harness can assert on what the model actually sent.
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "cf/mcp/stdio_server.hpp"

namespace {

using nlohmann::json;

const char* kProtocolVersion = "2025-06-18";

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : fallback;
}

void record_call(const std::string& tool, const json& arguments) {
  const std::string path = env_or("CF_MCP_CALLS_FILE", "");
  if (path.empty()) {
    return;
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  json entry = {{"ts_ms", now}, {"tool", tool}, {"arguments", arguments}};
  std::ofstream out(path, std::ios::app);
  if (out) {
    out << entry.dump() << "\n";
  }
}

json text_result(const std::string& text, bool is_error = false) {
  return json{{"content", json::array({json{{"type", "text"}, {"text", text}}})},
              {"isError", is_error}};
}

json tool_definitions() {
  return json::array({
      json{{"name", "submit_work"},
           {"description",
            "Ends the coder turn and hands the work back to the orchestrator. "
            "Call this exactly once when the task is finished."},
           {"inputSchema",
            json{{"type", "object"},
                 {"properties",
                  json{{"summary", json{{"type", "string"},
                                        {"description", "What was changed and why"}}},
                       {"defect_ids",
                        json{{"type", "array"},
                             {"items", json{{"type", "string"}}},
                             {"description", "Defect IDs addressed by this work"}}}}},
                 {"required", json::array({"summary", "defect_ids"})},
                 {"additionalProperties", false}}}},
      json{{"name", "submit_review"},
           {"description", "Ends the reviewer turn with a structured verdict."},
           {"inputSchema",
            json{{"type", "object"},
                 {"properties",
                  json{{"verdict_json", json{{"type", "object"}}}}},
                 {"required", json::array({"verdict_json"})},
                 {"additionalProperties", false}}}},
      json{{"name", "spike_echo"},
           {"description", "Echoes back the text argument. Used to test tool calling."},
           {"inputSchema",
            json{{"type", "object"},
                 {"properties", json{{"text", json{{"type", "string"}}}}},
                 {"required", json::array({"text"})},
                 {"additionalProperties", false}}}},
      json{{"name", "spike_error"},
           {"description", "Always returns a tool error result."},
           {"inputSchema", json{{"type", "object"}, {"properties", json::object()},
                                {"additionalProperties", false}}}},
      json{{"name", "spike_timeout"},
           {"description", "Blocks for the given number of seconds before replying."},
           {"inputSchema",
            json{{"type", "object"},
                 {"properties", json{{"seconds", json{{"type", "number"}}}}},
                 {"required", json::array({"seconds"})},
                 {"additionalProperties", false}}}},
      json{{"name", "spike_invalid_json"},
           {"description", "Writes malformed JSON to the transport."},
           {"inputSchema", json{{"type", "object"}, {"properties", json::object()},
                                {"additionalProperties", false}}}},
  });
}

json handle_request(const json& request) {
  const std::string method = request.value("method", "");
  const json params = request.value("params", json::object());

  if (method == "initialize") {
    const std::string client_version = params.value("protocolVersion", kProtocolVersion);
    return json{{"jsonrpc", "2.0"},
                {"id", request.value("id", json())},
                {"result",
                 json{{"protocolVersion", client_version},
                      {"capabilities", json{{"tools", json::object()}}},
                      {"serverInfo",
                       json{{"name", env_or("CF_MCP_NAME", "cf-mcp-spike")},
                            {"version", "0.1.0"}}}}}};
  }
  if (method == "notifications/initialized" || method == "initialized") {
    return json();
  }
  if (method == "ping") {
    return json{{"jsonrpc", "2.0"}, {"id", request.value("id", json())}, {"result", json::object()}};
  }
  if (method == "tools/list") {
    return json{{"jsonrpc", "2.0"},
                {"id", request.value("id", json())},
                {"result", json{{"tools", tool_definitions()}}}};
  }
  if (method == "tools/call") {
    const std::string tool = params.value("name", "");
    const json arguments = params.value("arguments", json::object());
    record_call(tool, arguments);

    if (tool == "submit_work") {
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", text_result("submit_work received: " +
                                         arguments.value("summary", ""))}};
    }
    if (tool == "submit_review") {
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", text_result("submit_review received")}};
    }
    if (tool == "spike_echo") {
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", text_result("echo: " + arguments.value("text", ""))}};
    }
    if (tool == "spike_error") {
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", text_result("intentional spike error", true)}};
    }
    if (tool == "spike_timeout") {
      const double seconds = arguments.value("seconds", 10.0);
      std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", text_result("timeout tool eventually replied")}};
    }
    if (tool == "spike_invalid_json") {
      const std::string raw =
          "{\"jsonrpc\":\"2.0\",\"id\":" + request.value("id", json()).dump() +
          ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"unterminated";
      return json{{"__raw__", raw}};
    }
    return json{{"jsonrpc", "2.0"},
                {"id", request.value("id", json())},
                {"error", json{{"code", -32602}, {"message", "unknown tool: " + tool}}}};
  }

  if (!request.contains("id")) {
    return json();  // unknown notification: ignore
  }
  return json{{"jsonrpc", "2.0"},
              {"id", request.value("id", json())},
              {"error", json{{"code", -32601}, {"message", "method not found: " + method}}}};
}

}  // namespace

int main() {
  spdlog::set_default_logger(spdlog::stderr_color_mt("cf-mcp"));
  spdlog::set_level(spdlog::level::info);
  cf::mcp::StdioServer server(&handle_request);
  return server.run();
}
