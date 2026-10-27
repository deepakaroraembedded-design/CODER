add_executable(cf_unit_tests
  result_test.cpp
  sse_parser_test.cpp
  hash_test.cpp
  run_store_test.cpp
  dispatcher_test.cpp
  state_machine_test.cpp
  scheduler_test.cpp
  review_policy_test.cpp
  milestone_loop_test.cpp
  refiner_test.cpp
  orchestrator_test.cpp
  approval_deadline_test.cpp
  llm_client_test.cpp
  plan_test.cpp
  profile_test.cpp
  process_test.cpp
  gate_runner_test.cpp
  rag_index_test.cpp
  catalog_test.cpp
  retriever_test.cpp
  rag_embedder_test.cpp
  rag_loader_test.cpp
  rag_html_test.cpp
  refmirror_test.cpp
  git_test.cpp
  recovery_test.cpp
  driver_test.cpp
  sandbox_test.cpp
  namespace_sandbox_test.cpp
  opencode_runner_test.cpp
  permission_broker_test.cpp
  role_config_test.cpp
  event_router_test.cpp
  session_idle_tracker_test.cpp
  session_agent_runner_test.cpp
  opencode_server_test.cpp
  opencode_sandbox_test.cpp
  proxy_test.cpp
  run_process_test.cpp
  prompt_library_test.cpp
  question_policy_test.cpp
  github_client_test.cpp
  pre_push_test.cpp
  github_handlers_test.cpp
  github_flow_test.cpp
  github_secrets_test.cpp
  github_app_auth_test.cpp
  licenses_test.cpp
  defect_export_test.cpp
  cli_test.cpp
  codeintel_test.cpp
  config_test.cpp
  codeintel_sync_test.cpp
  context_packer_test.cpp
  fault_injection_test.cpp
  treesitter_backend_test.cpp
  treesitter_python_test.cpp
  treesitter_go_test.cpp
  mcp_tools_test.cpp
  eval_test.cpp
  eval_benchmark_test.cpp
  eval_run_executor_test.cpp
  rag_eval_test.cpp
  github_reviewers_test.cpp)
target_link_libraries(cf_unit_tests PRIVATE
  cf_agents
  cf_store
  cf_orch
  cf_llm
  cf_plan
  cf_gates
  cf_rag
  cf_codeintel
  cf_mcp_tools
  cf_refmirror
  cf_git
  cf_cli
  cf_sandbox
  cf_github
  cf_eval
  cf_common
  GTest::gtest_main)

# The CLI smoke test runs the built cf binary.
target_compile_definitions(cf_unit_tests PRIVATE
  CF_BINARY_DIR="${CMAKE_BINARY_DIR}"
  CF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(cf_unit_tests cf cf-eval)

include(GoogleTest)
# PRE_TEST discovery keeps the build from executing the test binary: sanitizer
# builds (TSan) cannot always run in the build environment.
gtest_discover_tests(cf_unit_tests DISCOVERY_MODE PRE_TEST)
