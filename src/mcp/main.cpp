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
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "cf/codeintel/symbol_index.hpp"
#include "cf/mcp/stdio_server.hpp"
#include "cf/mcp/tools.hpp"
#include "cf/rag/embedder.hpp"
#include "cf/rag/loader.hpp"
#include "cf/rag/retriever.hpp"
#include "cf/store/run_store.hpp"
#include "cf/telemetry/logging.hpp"

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
    out.flush();
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
  std::string reference_docs_dir;
  std::string reference_versions_file;
  std::string designs_dir;
  std::string patterns_dir;
  std::string codeintel_db;
  std::string profile_path;
  std::string rules_dir;
  std::string profile_build_system;
  std::string profile_test_framework;
  std::string build_dir;
  std::string standard;
  std::string standard_var;
  std::string product;
  std::string language;
  std::string name;
  std::string rag_index = "hnswlib";
  double rag_bm25_weight = 1.0;
  double rag_vector_weight = 1.0;
  std::string rag_precedence;
  std::string embeddings_model;
  std::string embeddings_base_url;
  std::string embeddings_api_key_env = "FIREWORKS_API_KEY";
  std::uint32_t waiver_deadline_days = 2;
  std::uint32_t top_k = 8;
  app.add_option("--role", role_text, "Agent role: coder, reviewer or qa")
      ->required();
  app.add_option("--run", run_id, "Run id");
  app.add_option("--milestone", milestone_id, "Milestone id");
  app.add_option("--db", db_path, "Run store database (optional)");
  app.add_option("--docs", docs_dir,
                 "Directory of Markdown documents to index (optional)");
  app.add_option("--reference-docs", reference_docs_dir,
                 "Offline reference documentation to index (optional)");
  app.add_option("--reference-versions", reference_versions_file,
                 "Pinned reference versions JSON (optional)");
  app.add_option("--designs", designs_dir,
                 "Directory of approved design packages to index (optional)");
  app.add_option("--patterns", patterns_dir,
                 "Directory of pattern catalog entries (optional)");
  app.add_option("--codeintel-db", codeintel_db,
                 "cf-codeintel symbol index database (optional; design section 6.5)");
  app.add_option("--profile", profile_path,
                 "Toolchain profile YAML for run_checks (optional)");
  app.add_option("--rules-dir", rules_dir,
                 "Company guardrail rules directory (for run_checks)");
  app.add_option("--profile-build-system", profile_build_system,
                 "Build system key inside the profile (for run_checks)");
  app.add_option("--profile-test-framework", profile_test_framework,
                 "Test framework key inside the profile (for run_checks)");
  app.add_option("--build-dir", build_dir,
                 "Build directory for run_checks (default: .cf-build)");
  app.add_option("--standard", standard,
                 "Language standard from the plan (for run_checks)");
  app.add_option("--standard-var", standard_var,
                 "Build-system standard variable (for run_checks)");
  app.add_option("--product", product,
                 "Product scope for RAG applicability (design section 6.3)");
  app.add_option("--language", language,
                 "Target language scope for RAG applicability");
  app.add_option("--name", name, "Server name reported to MCP");
  app.add_option("--top-k", top_k,
                 "Default number of results for search_guidelines (default 8)");
  app.add_option("--rag-index", rag_index,
                 "Vector index: hnswlib (default), faiss, faiss-ivfpq or brute-force");
  app.add_option("--rag-bm25-weight", rag_bm25_weight,
                 "Hybrid BM25 weight (default 1.0)");
  app.add_option("--rag-vector-weight", rag_vector_weight,
                 "Hybrid vector weight (default 1.0)");
  app.add_option("--rag-precedence", rag_precedence,
                 "Comma-separated document precedence, highest first");
  app.add_option("--embeddings-model", embeddings_model,
                 "Embedding model for hybrid retrieval (optional)");
  app.add_option("--embeddings-base-url", embeddings_base_url,
                 "OpenAI-compatible embeddings endpoint");
  app.add_option("--embeddings-api-key-env", embeddings_api_key_env,
                 "Environment variable holding the embeddings API key");
  app.add_option("--waiver-deadline-days", waiver_deadline_days,
                 "Business-day deadline for request_waiver (default 2)");
  CLI11_PARSE(app, argc, argv);

  spdlog::set_default_logger(spdlog::stderr_color_mt("cf-mcp"));
  cf::telemetry::configure_json_logging();

  const auto role = cf::mcp::tool_role_from_string(role_text);
  if (!role) {
    spdlog::error("unknown role '{}': expected coder, reviewer or qa", role_text);
    return 2;
  }
  cf::telemetry::ScopedContext log_context(run_id, milestone_id, role_text);
  if (rag_index != "hnswlib" && rag_index != "brute-force" &&
      rag_index != "faiss" && rag_index != "faiss-ivfpq") {
    spdlog::error("--rag-index must be hnswlib, faiss, faiss-ivfpq or brute-force");
    return 2;
  }
  if ((rag_bm25_weight < 0.0) || (rag_vector_weight < 0.0) ||
      (rag_bm25_weight == 0.0 && rag_vector_weight == 0.0)) {
    spdlog::error("hybrid weights must be non-negative with one positive");
    return 2;
  }
  if (top_k == 0) {
    spdlog::error("--top-k must be positive");
    return 2;
  }

  cf::rag::RetrieverOptions retriever_options;
  retriever_options.hnsw = rag_index == "hnswlib";
  retriever_options.faiss = rag_index == "faiss";
  retriever_options.faiss_ivfpq = rag_index == "faiss-ivfpq";
  retriever_options.bm25_weight = rag_bm25_weight;
  retriever_options.vector_weight = rag_vector_weight;
  if (!rag_precedence.empty()) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= rag_precedence.size()) {
      const std::size_t comma = rag_precedence.find(',', start);
      const std::size_t end =
          comma == std::string::npos ? rag_precedence.size() : comma;
      if (end > start) {
        names.push_back(rag_precedence.substr(start, end - start));
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    auto types = cf::rag::doc_types_from_names(names);
    if (!types) {
      spdlog::error("invalid --rag-precedence: {}", types.error().message);
      return 2;
    }
    retriever_options.precedence = std::move(*types);
  }

  std::vector<cf::rag::PatternEntry> catalog;
  if (!patterns_dir.empty()) {
    auto loaded = cf::rag::load_pattern_catalog(patterns_dir);
    if (!loaded) {
      spdlog::error("cannot load the pattern catalog: {}", loaded.error().message);
      return 3;
    }
    catalog = std::move(*loaded);
  }

  std::shared_ptr<cf::rag::InMemoryRetriever> retriever;
  std::shared_ptr<cf::llm::FireworksClient> embedding_client;
  if (!docs_dir.empty() || !reference_docs_dir.empty() ||
      !designs_dir.empty() || !catalog.empty()) {
    retriever =
        std::make_shared<cf::rag::InMemoryRetriever>(retriever_options);
    if (!embeddings_model.empty()) {
      const char* key = std::getenv(embeddings_api_key_env.c_str());
      if (key == nullptr || *key == '\0') {
        spdlog::warn("{} is not set; hybrid retrieval uses BM25 only",
                     embeddings_api_key_env);
      } else {
        cf::llm::ClientOptions embedding_options;
        embedding_options.base_url = embeddings_base_url;
        embedding_options.api_key = key;
        embedding_client = std::make_shared<cf::llm::FireworksClient>(
            embedding_options, std::make_unique<cf::llm::CprTransport>());
        retriever->set_embedder(std::make_shared<cf::rag::FireworksEmbedder>(
            *embedding_client, embeddings_model, run_id));
      }
    }
    for (const auto& directory :
         {docs_dir, reference_docs_dir, designs_dir}) {
      if (directory.empty()) {
        continue;
      }
      auto documents = cf::rag::load_document_dir(directory);
      if (!documents) {
        spdlog::error("cannot load documents from {}: {}", directory,
                      documents.error().message);
        return 3;
      }
      for (auto& document : *documents) {
        auto added = retriever->add_document(std::move(document));
        if (!added) {
          spdlog::error("cannot index documents: {}", added.error().message);
          return 3;
        }
      }
      spdlog::info("indexed {} document(s) from {}", documents->size(),
                   directory);
    }
    // The pattern catalog is also part of the retrieval collection
    // (design section 6.2).
    for (const auto& entry : catalog) {
      auto added = retriever->add_document(cf::rag::pattern_document(entry));
      if (!added) {
        spdlog::error("cannot index pattern {}: {}", entry.id,
                      added.error().message);
        return 3;
      }
    }
  }

  std::map<std::string, std::string, std::less<>> reference_versions;
  if (!reference_versions_file.empty()) {
    std::ifstream in(reference_versions_file, std::ios::binary);
    const json document = json::parse(in, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      spdlog::error("cannot read the reference versions file: {}",
                    reference_versions_file);
      return 7;
    }
    const auto versions = document.find("versions");
    if (versions != document.end() && versions->is_object()) {
      for (const auto& [library, version] : versions->items()) {
        if (version.is_string()) {
          reference_versions[library] = version.get<std::string>();
        }
      }
    }
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

  std::unique_ptr<cf::codeintel::SymbolIndex> symbols;
  if (!codeintel_db.empty()) {
    auto opened = cf::codeintel::SymbolIndex::open(codeintel_db);
    if (!opened) {
      spdlog::error("cannot open the symbol index: {}", opened.error().message);
      return 5;
    }
    symbols = std::move(*opened);
  }

  std::unique_ptr<cf::process::LocalProcessExecutor> check_executor;
  cf::mcp::GateCheckContext check_context;
  const bool checks_enabled = !profile_path.empty();
  if (checks_enabled) {
    auto profile = cf::gates::load_profile_file(profile_path);
    if (!profile) {
      spdlog::error("cannot load the toolchain profile: {}",
                    profile.error().message);
      return 6;
    }
    check_context.profile = std::move(*profile);
    check_context.rules_dir = rules_dir;
    check_context.language = language;
    check_context.build_system = profile_build_system;
    check_context.test_framework = profile_test_framework;
    check_context.standard = standard;
    check_context.standard_var = standard_var;
    check_context.worktree = std::filesystem::current_path();
    check_context.build_dir = build_dir.empty()
                                  ? check_context.worktree / ".cf-build"
                                  : std::filesystem::path(build_dir);
    check_executor = std::make_unique<cf::process::LocalProcessExecutor>();
    check_context.executor = check_executor.get();
  }

  cf::mcp::ToolDependencies dependencies;
  dependencies.run_id = run_id;
  dependencies.milestone_id = milestone_id;
  dependencies.role = role_text;
  dependencies.waiver_deadline_business_days = waiver_deadline_days;
  dependencies.context.product = product;
  dependencies.context.language = language;
  dependencies.retriever = retriever.get();
  dependencies.default_top_k = top_k;
  dependencies.store = store.get();
  dependencies.patterns = &catalog;
  dependencies.symbols = symbols.get();
  if (checks_enabled) {
    dependencies.gate_checks = &check_context;
  }
  if (!reference_versions.empty()) {
    dependencies.reference_versions = &reference_versions;
  }
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
