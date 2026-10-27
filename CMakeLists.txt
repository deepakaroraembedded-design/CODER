cmake_minimum_required(VERSION 3.25)

project(coder-framework
  VERSION 0.1.0
  DESCRIPTION "Coder Framework (CF): multi-agent, guardrail-driven code generation"
  # C is for the vendored tree-sitter grammars (third_party/tree-sitter-c);
  # framework code stays strict C++17.
  LANGUAGES C CXX)

# The framework itself is strict C++17 (design section 4.1); the code the
# agents generate follows the plan's target block and is not built here.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(hnswlib CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Warnings apply to framework code only, never to third-party headers.
add_library(cf_project_options INTERFACE)
target_compile_options(cf_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror>)

# cf-common: shared utilities (Result contract, hashing, process execution)
add_library(cf_common STATIC
  src/common/hash.cpp
  src/common/process.cpp)
target_include_directories(cf_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cf_common PUBLIC
  cf_project_options
  tl::expected
  OpenSSL::Crypto
  Threads::Threads)

# cf-agents: opencode adapter (HTTP + SSE client)
add_library(cf_agents STATIC
  src/agents/opencode_client.cpp
  src/agents/permission_broker.cpp)
target_link_libraries(cf_agents PUBLIC
  cf_common
  Boost::system
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

# cf-mcp: MCP server over stdio
add_library(cf_mcp_stdio STATIC
  src/mcp/stdio_server.cpp)
target_link_libraries(cf_mcp_stdio PUBLIC
  cf_common
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

# cf-mcp: the design's tool surface (design section 9.1)
add_library(cf_mcp_tools STATIC
  src/mcp/tools.cpp)
target_link_libraries(cf_mcp_tools PUBLIC
  cf_common
  cf_codeintel
  cf_rag
  cf_store
  nlohmann_json::nlohmann_json)

add_executable(cf-mcp src/mcp/main.cpp)
target_link_libraries(cf-mcp PRIVATE
  cf_mcp_stdio
  cf_mcp_tools
  cf_codeintel
  cf_rag
  cf_store
  CLI11::CLI11
  spdlog::spdlog)

# Phase 0a harness spike MCP stub (scenarios H1-H15); the real server is
# cf-mcp above.
add_executable(cf-mcp-spike src/mcp/spike_main.cpp)
target_link_libraries(cf-mcp-spike PRIVATE cf_mcp_stdio cf_common)

# cf-store: SQLite run store and side-effect protocol persistence
add_library(cf_store STATIC
  src/store/run_store.cpp)
target_link_libraries(cf_store PUBLIC
  cf_common
  SQLiteCpp
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

# cf-orch: orchestrator skeleton (side-effect dispatcher, state machines,
# scheduler; design sections 6.12, 7, 6.13)
add_library(cf_orch STATIC
  src/orch/dispatcher.cpp
  src/orch/state_machine.cpp
  src/orch/approval_deadline.cpp
  src/orch/scheduler.cpp
  src/orch/review_policy.cpp
  src/orch/milestone_loop.cpp
  src/orch/orchestrator.cpp
  src/orch/escalation.cpp
  src/orch/recovery.cpp
  src/orch/driver.cpp
  src/orch/opencode_runner.cpp
  src/orch/opencode_session_driver.cpp
  src/orch/session_idle_tracker.cpp
  src/orch/role_config.cpp
  src/orch/event_router.cpp
  src/orch/opencode_server.cpp
  src/orch/session_agent_runner.cpp
  src/orch/opencode_factory.cpp
  src/orch/run_process.cpp
  src/orch/refiner.cpp
  src/orch/prompt_library.cpp
  src/orch/question_policy.cpp
  src/orch/github_handlers.cpp
  src/orch/github_flow.cpp
  src/orch/defect_export.cpp
  src/orch/codeintel_sync.cpp)
target_link_libraries(cf_orch PUBLIC
  cf_store
  cf_plan
  cf_gates
  cf_git
  cf_sandbox
  cf_agents
  cf_rag
  cf_codeintel
  cf_github)

# cf-llm: Fireworks / OpenAI-compatible client (design section 6.6)
add_library(cf_llm STATIC
  src/llm/client.cpp)
target_link_libraries(cf_llm PUBLIC
  cf_common
  cpr::cpr
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

# cf-plan: implementation plan parser and validator (design section 6.2)
add_library(cf_plan STATIC
  src/plan/plan.cpp)
target_link_libraries(cf_plan PUBLIC
  cf_common
  yaml-cpp::yaml-cpp)

# cf-eval: golden evaluation suite harness and model benchmark (design
# sections 17.2, 18.2 Phase 0b)
add_library(cf_eval STATIC
  src/eval/suite.cpp
  src/eval/outcome.cpp
  src/eval/score.cpp
  src/eval/report.cpp
  src/eval/benchmark.cpp
  src/eval/hidden.cpp
  src/eval/run_executor.cpp)
target_link_libraries(cf_eval PUBLIC
  cf_common
  cf_plan
  cf_store
  cf_gates
  cf_git
  cf_orch
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp
  spdlog::spdlog)

add_executable(cf-eval src/eval/main.cpp)
target_link_libraries(cf-eval PRIVATE cf_eval cf_rag CLI11::CLI11)

# cf-rag: retrieval engine (design section 6.3)
add_library(cf_rag STATIC
  src/rag/types.cpp
  src/rag/chunker.cpp
  src/rag/bm25.cpp
  src/rag/vector_index.cpp
  src/rag/embedder.cpp
  src/rag/loader.cpp
  src/rag/catalog.cpp
  src/rag/html.cpp
  src/rag/injection_scan.cpp
  src/rag/evaluation.cpp
  src/rag/retriever.cpp)
target_link_libraries(cf_rag PUBLIC
  cf_common
  cf_llm
  hnswlib::hnswlib
  yaml-cpp::yaml-cpp)

# cf-git: repository layer (design sections 6.8.3, 10.4.5)
add_library(cf_git STATIC
  src/git/repository.cpp)
target_link_libraries(cf_git PUBLIC cf_common)

# The vendored tree-sitter grammars (third_party/tree-sitter-*, pinned in
# third_party/VERSIONS): generated parser code, so no project warnings apply.
add_library(tree_sitter_c_grammar STATIC
  third_party/tree-sitter-c/src/parser.c)
add_library(tree_sitter_python_grammar STATIC
  third_party/tree-sitter-python/src/parser.c
  third_party/tree-sitter-python/src/scanner.c)
add_library(tree_sitter_go_grammar STATIC
  third_party/tree-sitter-go/src/parser.c)

# cf-codeintel: code intelligence (design section 6.5)
add_library(cf_codeintel STATIC
  src/codeintel/types.cpp
  src/codeintel/backend.cpp
  src/codeintel/symbol_index.cpp
  src/codeintel/indexer.cpp
  src/codeintel/context_packer.cpp
  src/codeintel/grammar_c.cpp
  src/codeintel/grammar_python.cpp
  src/codeintel/grammar_go.cpp
  src/codeintel/treesitter_backend.cpp)
target_link_libraries(cf_codeintel PUBLIC
  cf_common
  SQLiteCpp
  unofficial::tree-sitter::tree-sitter
  tree_sitter_c_grammar
  tree_sitter_python_grammar
  tree_sitter_go_grammar)

# cf-sandbox: Docker and daemon-free namespace executors (design section 6.8)
add_library(cf_sandbox STATIC
  src/sandbox/docker.cpp
  src/sandbox/namespaces.cpp
  src/sandbox/proxy.cpp)
target_link_libraries(cf_sandbox PUBLIC cf_common Threads::Threads)

# cf-github: REST client and pre-push checks (design sections 6.10, 10.4)
add_library(cf_github STATIC
  src/github/client.cpp
  src/github/codeowners.cpp
  src/github/pre_push.cpp
  src/github/secrets.cpp
  src/github/app_auth.cpp
  src/github/licenses.cpp)
target_link_libraries(cf_github PUBLIC
  cf_common
  cf_git
  cf_llm
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp)

# cf-cli: command implementations and the cf binary (design section 9.2)
add_library(cf_cli STATIC
  src/cli/commands.cpp
  src/cli/config.cpp)
target_link_libraries(cf_cli PUBLIC
  cf_common
  cf_store
  cf_plan
  cf_gates
  cf_orch
  cf_git
  cf_codeintel
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp)

add_executable(cf src/cli/main.cpp)
target_link_libraries(cf PRIVATE cf_cli CLI11::CLI11)

# cf-refmirror: offline reference documentation mirroring (design 6.3.1)
add_library(cf_refmirror STATIC
  src/refmirror/manifest.cpp
  src/refmirror/mirror.cpp)
target_link_libraries(cf_refmirror PUBLIC
  cf_common
  cf_rag
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json)

add_executable(cf-refmirror src/refmirror/main.cpp)
target_link_libraries(cf-refmirror PRIVATE
  cf_refmirror
  cf_llm
  CLI11::CLI11
  spdlog::spdlog)

# cf-gates: toolchain profiles and the staged gate runner (design section 6.7)
add_library(cf_gates STATIC
  src/gates/profile.cpp
  src/gates/runner.cpp)
target_link_libraries(cf_gates PUBLIC
  cf_common
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json)

# Phase 0a harness compatibility spike (scenarios H1-H15, design section 6.6.2)
add_executable(cf-harness-spike
  tests/harness_compat/main.cpp
  tests/harness_compat/harness.cpp
  tests/harness_compat/scenarios.cpp)
target_link_libraries(cf-harness-spike PRIVATE
  cf_agents
  cf_common
  CLI11::CLI11
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

enable_testing()
add_subdirectory(tests/unit)
