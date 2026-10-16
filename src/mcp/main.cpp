// cf-mcp: the MCP tool server (design sections 6.4, 9.1).
//
// Implements the design's role-filtered tool surface over MCP stdio:
//
//   cf-mcp --role=coder|reviewer|qa [--run R12] [--milestone M3]
//          [--db runs/R12/run.db] [--docs docs/]
//
// Every tools/call is appended as one JSON line to CF_MCP_CALLS_FILE when that
// environment variable is set, so the orchestrator can observe the agent's
// structured submissions. The Phase 0a harness spike lives in cf-mcp-spike.
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "cf/mcp/stdio_server.hpp"
#include "cf/mcp/tools.hpp"
#include "cf/rag/loader.hpp"
#include "cf/rag/retriever.hpp"
#include "cf/store/run_store.hpp"

namespace {

using nlohmann::json;

const char* kProtocolVersion = "2025-06-18";

void record_call(const std::string& tool, const json& arguments) {
  const char* path = std::getenv("CF_MCP_CALLS_FILE");
  if (path == nullptr || *path == '\0') {
    return;
  }
  json entry = {{"tool", tool}, {"arguments", arguments}};
  std::ofstream out(path, std::ios::app);
  if (out) {
    out << entry.dump() << "\n";
  }
}

json text_result(const std::string& text, bool is_error) {
  return json{{"content", json::array({json{{"type", "text"}, {"text", text}}})},
              {"isError", is_error}};
}

json tool_result_json(const cf::mcp::ToolResult& result) {
  json out = text_result(result.text, result.is_error);
  if (!result.structured.is_null()) {
    out["structuredContent"] = result.structured;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"cf-mcp: Coder Framework MCP tool server"};
  std::string role_text;
  std::string run_id;
  std::string milestone_id;
  std::string db_path;
  std::string docs_dir;
  std::string name;
  app.add_option("--role", role_text, "Agent role: coder, reviewer or qa")
      ->required();
  app.add_option("--run", run_id, "Run id");
  app.add_option("--milestone", milestone_id, "Milestone id");
  app.add_option("--db", db_path, "Run store database (optional)");
  app.add_option("--docs", docs_dir,
                 "Directory of Markdown documents to index (optional)");
  app.add_option("--name", name, "Server name reported to MCP");
  CLI11_PARSE(app, argc, argv);

  spdlog::set_default_logger(spdlog::stderr_color_mt("cf-mcp"));
  spdlog::set_level(spdlog::level::info);

  const auto role = cf::mcp::tool_role_from_string(role_text);
  if (!role) {
    spdlog::error("unknown role '{}': expected coder, reviewer or qa", role_text);
    return 2;
  }

  std::shared_ptr<cf::rag::InMemoryRetriever> retriever;
  if (!docs_dir.empty()) {
    retriever = std::make_shared<cf::rag::InMemoryRetriever>();
    auto documents = cf::rag::load_document_dir(docs_dir);
    if (!documents) {
      spdlog::error("cannot load documents: {}", documents.error().message);
      return 3;
    }
    for (auto& document : *documents) {
      auto added = retriever->add_document(std::move(document));
      if (!added) {
        spdlog::error("cannot index documents: {}", added.error().message);
        return 3;
      }
    }
    spdlog::info("indexed {} document(s) from {}", documents->size(), docs_dir);
  }

  std::unique_ptr<cf::store::RunStore> store;
  if (!db_path.empty()) {
    auto opened = cf::store::RunStore::open(db_path);
    if (!opened) {
      spdlog::error("cannot open run store: {}", opened.error().message);
      return 4;
    }
    store = std::move(*opened);
  }

  cf::mcp::ToolDependencies dependencies;
  dependencies.run_id = run_id;
  dependencies.milestone_id = milestone_id;
  dependencies.retriever = retriever.get();
  dependencies.store = store.get();
  const cf::mcp::ToolRegistry registry =
      cf::mcp::build_tool_registry(std::move(dependencies));

  const std::string server_name = name.empty() ? "cf-mcp" : name;

  const cf::mcp::StdioServer::Handler handler = [&](const json& request) -> json {
    const std::string method = request.value("method", "");
    const json params = request.value("params", json::object());

    if (method == "initialize") {
      const std::string client_version =
          params.value("protocolVersion", kProtocolVersion);
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result",
                   json{{"protocolVersion", client_version},
                        {"capabilities", json{{"tools", json::object()}}},
                        {"serverInfo",
                         json{{"name", server_name}, {"version", "0.2.0"}}}}}};
    }
    if (method == "notifications/initialized" || method == "initialized") {
      return json();
    }
    if (method == "ping") {
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", json::object()}};
    }
    if (method == "tools/list") {
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", registry.list_for_role(*role)}};
    }
    if (method == "tools/call") {
      const std::string tool = params.value("name", "");
      const json arguments = params.value("arguments", json::object());
      record_call(tool, arguments);
      const cf::mcp::ToolResult result = registry.call(*role, tool, arguments);
      return json{{"jsonrpc", "2.0"},
                  {"id", request.value("id", json())},
                  {"result", tool_result_json(result)}};
    }

    if (!request.contains("id")) {
      return json();  // unknown notification: ignore
    }
    return json{{"jsonrpc", "2.0"},
                {"id", request.value("id", json())},
                {"error",
                 json{{"code", -32601}, {"message", "method not found: " + method}}}};
  };

  cf::mcp::StdioServer server(handler);
  return server.run();
}
