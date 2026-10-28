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
find_package(ZLIB REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(hnswlib CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Optional Clang LibTooling back end for C/C++ code intelligence (design
# section 6.5). Set LLVM_DIR/Clang_DIR (or CMAKE_PREFIX_PATH) to a Clang 17
# installation to enable it; otherwise the tree-sitter fallback is used.
# LLVM's prebuilt CMake exports reference zstd/terminfo imported targets even
# when the matching development symlinks are not installed. Point those
# targets at the runtime libraries when necessary; LibTooling only needs to
# link them, not include their headers.
if(NOT TARGET zstd::libzstd_shared)
  find_file(CF_ZSTD_SHARED_LIBRARY NAMES libzstd.so.1
    PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu)
  if(CF_ZSTD_SHARED_LIBRARY)
    add_library(zstd::libzstd_shared SHARED IMPORTED)
    set_target_properties(zstd::libzstd_shared PROPERTIES
      IMPORTED_LOCATION "${CF_ZSTD_SHARED_LIBRARY}")
  endif()
endif()
if(NOT TARGET Terminfo::terminfo)
  find_file(CF_TERMINFO_SHARED_LIBRARY NAMES libtinfo.so.6
    PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu)
  if(CF_TERMINFO_SHARED_LIBRARY)
    add_library(Terminfo::terminfo SHARED IMPORTED)
    set_target_properties(Terminfo::terminfo PROPERTIES
      IMPORTED_LOCATION "${CF_TERMINFO_SHARED_LIBRARY}")
  endif()
endif()

option(CF_ENABLE_CLANG_CODEINTEL "Build the Clang LibTooling back end when available" ON)
if(CF_ENABLE_CLANG_CODEINTEL)
  find_package(LLVM 17 CONFIG QUIET)
  if(LLVM_FOUND)
    find_package(Clang 17 CONFIG QUIET)
  endif()
endif()

# Warnings apply to framework code only, never to third-party headers.
add_library(cf_project_options INTERFACE)
target_compile_options(cf_project_options INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror>)

# cf-common: shared utilities (Result contract, hashing, process execution)
add_library(cf_common STATIC
  src/common/archive.cpp
  src/common/hash.cpp
  src/common/process.cpp)
target_include_directories(cf_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cf_common PUBLIC
  cf_project_options
  tl::expected
  OpenSSL::Crypto
  ZLIB::ZLIB
  Threads::Threads)

# cf-telemetry: structured JSON logging with run/milestone/agent/trace context
# (design sections 6.11, 14)
add_library(cf_telemetry STATIC
  src/telemetry/logging.cpp
  src/telemetry/metrics.cpp)
target_link_libraries(cf_telemetry PUBLIC
  cf_project_options
  cf_common
  cf_llm
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

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
  cf_telemetry
  nlohmann_json::nlohmann_json
  spdlog::spdlog)

# cf-mcp: the design's tool surface (design section 9.1)
add_library(cf_mcp_tools STATIC
  src/mcp/tools.cpp)
target_link_libraries(cf_mcp_tools PUBLIC
  cf_common
  cf_codeintel
  cf_gates
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
  cf_llm
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
  src/orch/run_summary.cpp
  src/orch/defect_export.cpp
  src/orch/codeintel_sync.cpp
  src/orch/waivers.cpp
  src/orch/metrics.cpp)
target_link_libraries(cf_orch PUBLIC
  cf_telemetry
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
  spdlog::spdlog
  OpenSSL::Crypto)

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
  cf_telemetry
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

# Optional FAISS backend (design section 6.3). Point FAISS_ROOT at a FAISS
# installation prefix; without it the hnswlib/brute-force indexes remain.
set(FAISS_ROOT "" CACHE PATH "Optional FAISS installation prefix")
find_path(FAISS_INCLUDE_DIR NAMES faiss/IndexHNSW.h
  HINTS "${FAISS_ROOT}/include")
find_library(FAISS_LIBRARY NAMES faiss
  HINTS "${FAISS_ROOT}/lib/x86_64-linux-gnu" "${FAISS_ROOT}/lib")
if(FAISS_INCLUDE_DIR AND FAISS_LIBRARY)
  find_package(OpenMP REQUIRED)
  find_file(FAISS_BLAS_LIBRARY NAMES libblas.so.3 libblas.so
    HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
  find_file(FAISS_LAPACK_LIBRARY NAMES liblapack.so.3 liblapack.so
    HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
  find_file(FAISS_GFORTRAN_LIBRARY NAMES libgfortran.so.5 libgfortran.so
    HINTS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
  if(FAISS_BLAS_LIBRARY AND FAISS_LAPACK_LIBRARY AND FAISS_GFORTRAN_LIBRARY)
    target_sources(cf_rag PRIVATE src/rag/faiss_index.cpp)
    target_include_directories(cf_rag SYSTEM PRIVATE "${FAISS_INCLUDE_DIR}")
    target_compile_definitions(cf_rag PUBLIC CF_HAS_FAISS=1)
    target_link_libraries(cf_rag PUBLIC
      "${FAISS_LIBRARY}"
      "${FAISS_BLAS_LIBRARY}"
      "${FAISS_LAPACK_LIBRARY}"
      "${FAISS_GFORTRAN_LIBRARY}"
      OpenMP::OpenMP_CXX)
    message(STATUS "cf-rag: FAISS HNSW backend enabled")
  endif()
endif()

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
add_library(tree_sitter_typescript_grammar STATIC
  third_party/tree-sitter-typescript/typescript/src/parser.c
  third_party/tree-sitter-typescript/typescript/src/scanner.c)
target_include_directories(tree_sitter_typescript_grammar PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-typescript/typescript/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-typescript/common)
add_library(tree_sitter_tsx_grammar STATIC
  third_party/tree-sitter-typescript/tsx/src/parser.c
  third_party/tree-sitter-typescript/tsx/src/scanner.c)
target_include_directories(tree_sitter_tsx_grammar PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-typescript/tsx/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-typescript/common)
add_library(tree_sitter_java_grammar STATIC
  third_party/tree-sitter-java/src/parser.c)
add_library(tree_sitter_cpp_grammar STATIC
  third_party/tree-sitter-cpp/src/parser.c
  third_party/tree-sitter-cpp/src/scanner.c)

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
  src/codeintel/grammar_typescript.cpp
  src/codeintel/grammar_java.cpp
  src/codeintel/grammar_cpp.cpp
  src/codeintel/treesitter_backend.cpp)
target_link_libraries(cf_codeintel PUBLIC
  cf_common
  SQLiteCpp
  unofficial::tree-sitter::tree-sitter
  tree_sitter_c_grammar
  tree_sitter_python_grammar
  tree_sitter_go_grammar
  tree_sitter_typescript_grammar
  tree_sitter_tsx_grammar
  tree_sitter_java_grammar
  tree_sitter_cpp_grammar)
if(LLVM_FOUND AND Clang_FOUND)
  target_sources(cf_codeintel PRIVATE src/codeintel/clang_backend.cpp)
  set_source_files_properties(src/codeintel/clang_backend.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-rtti")
  target_include_directories(cf_codeintel SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS})
  target_compile_definitions(cf_codeintel PUBLIC CF_HAS_CLANG=1)
  target_compile_definitions(cf_codeintel PRIVATE
    CF_CLANG_RESOURCE_DIR="${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
  target_link_libraries(cf_codeintel PUBLIC
    clangTooling
    clangFrontend
    clangSerialization
    clangDriver
    clangParse
    clangSema
    clangAnalysis
    clangAST
    clangBasic
    clangLex
    clangEdit
    clangSupport)
  message(STATUS "cf-codeintel: Clang LibTooling back end enabled (${LLVM_PACKAGE_VERSION})")

  # Company guardrail checks (design section 6.7.1). The pinned LLVM package is
  # static, so use a small clang-tidy-compatible driver rather than relying on
  # clang-tidy's plugin loader.
  add_library(cf_tidy STATIC src/tidy/checks.cpp)
  target_link_libraries(cf_tidy PUBLIC
    cf_common
    cf_project_options
    yaml-cpp::yaml-cpp
    clangTooling
    clangFrontend
    clangSerialization
    clangDriver
    clangParse
    clangSema
    clangAnalysis
    clangAST
    clangBasic
    clangLex
    clangEdit
    clangSupport)
  target_include_directories(cf_tidy SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS})
  target_compile_definitions(cf_tidy PUBLIC CF_HAS_TIDY=1)
  target_compile_definitions(cf_tidy PRIVATE
    CF_CLANG_RESOURCE_DIR="${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}")
  set_source_files_properties(src/tidy/checks.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-rtti")

  add_executable(cf-tidy src/tidy/main.cpp)
  target_link_libraries(cf-tidy PRIVATE cf_tidy cf_project_options)
  set_source_files_properties(src/tidy/main.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-rtti")
else()
  message(STATUS "cf-codeintel: Clang LibTooling back end disabled; tree-sitter fallback only")
  message(STATUS "cf-codeintel: cf-tidy company checks disabled")
endif()

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
  src/github/rulesets.cpp
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
  cf_telemetry
  cf_common
  cf_store
  cf_plan
  cf_gates
  cf_orch
  cf_git
  cf_codeintel
  cf_refmirror
  cf_llm
  cf_rag
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp)

add_executable(cf src/cli/main.cpp)
target_link_libraries(cf PRIVATE cf_cli CLI11::CLI11)

# cf-refmirror: offline reference documentation mirroring (design 6.3.1)
add_library(cf_refmirror STATIC
  src/refmirror/manifest.cpp
  src/refmirror/mirror.cpp
  src/refmirror/versions.cpp)
target_link_libraries(cf_refmirror PUBLIC
  cf_telemetry
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
