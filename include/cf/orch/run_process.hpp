// cf-orch: the run process (design sections 5.3, 6.1, 7.1).
//
// Composes a whole run: the production session-driver factory (or an injected
// one for tests), the workspace driver, and the run orchestrator, then drives
// the run until it is Done, Escalated, Aborted or waiting on a person.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cf/codeintel/backend.hpp"
#include "cf/common/process.hpp"
#include "cf/git/repository.hpp"
#include "cf/github/pre_push.hpp"
#include "cf/llm/client.hpp"
#include "cf/orch/driver.hpp"
#include "cf/orch/opencode_factory.hpp"
#include "cf/orch/orchestrator.hpp"
#include "cf/orch/session_agent_runner.hpp"

namespace cf::orch {

struct RunProcessConfig {
  std::string run_id;
  std::filesystem::path workspace_root;
  std::string integration_branch;  // cf/<run>/integration
  std::string base_branch = "main";

  // opencode
  std::string opencode_binary = "opencode";
  std::string provider_base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key_env = "FIREWORKS_API_KEY";
  std::map<std::string, std::string, std::less<>> role_models;
  // Extra opencode model options per role (see RoleConfigSpec).
  std::map<std::string, nlohmann::json, std::less<>> role_options;
  std::string mcp_command;  // path to the cf-mcp binary
  std::string docs_dir;
  // Mirrored offline reference documentation and the pinned version map
  // (design section 6.3.1); empty when reference_docs is not configured.
  std::string reference_docs_dir;
  std::string reference_versions_file;
  // Pattern catalog directory (design section 6.2); empty omits it.
  std::string patterns_dir;
  // Retrieval settings (design sections 6.3, 15); unset keeps the built-in
  // behaviour.
  std::optional<std::string> rag_index;
  std::optional<std::uint32_t> rag_top_k;
  std::optional<double> rag_bm25_weight;
  std::optional<double> rag_vector_weight;
  std::optional<std::vector<std::string>> rag_precedence;
  // Embeddings for hybrid retrieval (design sections 6.3, 15); empty leaves
  // the retriever BM25-only.
  std::string embeddings_model;
  std::string embeddings_base_url;
  std::string embeddings_api_key_env = "FIREWORKS_API_KEY";
  std::vector<std::pair<std::string, std::string>> environment;
  std::chrono::milliseconds turn_timeout{600000};

  // Sandbox the opencode servers (design section 6.8); empty runs them on the
  // host. The container needs a network for the model endpoint.
  bool agent_sandbox_namespaces = false;
  std::string ns_agent_binary = "cf-ns-agent";
  std::string agent_sandbox_image;
  std::string agent_sandbox_network;
  std::string agent_sandbox_binary = "opencode";
  std::string docker_binary = "docker";
  std::string sandbox_mcp_binary_host;  // cf-mcp binary mounted into the image
  // Egress allow-list for the agent containers (design section 6.8.2);
  // empty leaves the container on the plain bridge network.
  std::vector<std::string> sandbox_allowed_hosts;

  // gates
  gates::Profile profile;
  std::string profile_language;
  std::string profile_build_system;
  std::string profile_test_framework;
  // Source file of the resolved profile; the run copies it into the run
  // directory so cf-mcp's run_checks can load the same trusted commands.
  std::string profile_path;
  // Company guardrail rules (Semgrep) copied into the run directory and
  // mounted for run_checks/gates; empty disables the swt.* profile steps.
  std::string rules_dir;
  std::string sandbox_image;
  std::chrono::milliseconds step_timeout{600000};
  // Review and gate resources (design section 15, review/sandbox sections).
  std::uint32_t full_review_threshold = 400;
  // Review findings of these severities block the milestone (design
  // sections 8.2, 15).
  std::vector<std::string> review_blocking_severities = {"blocker", "major"};
  // Business-day deadline for an agent waiver request (design section 12.1).
  std::uint32_t waiver_business_days = 2;
  double gate_cpus = 0.0;      // 0: no limit
  std::uint64_t gate_memory_mb = 0;  // 0: no limit

  // orchestration
  OrchestratorConfig orchestrator;
  uint32_t max_system_validation_attempts = 1;
  // Startup recovery (design section 6.12.4): the lease lifetime and the
  // in-flight grace period before operations left by an older epoch are
  // reconciled (design section 6.12.6 rule 3).
  std::chrono::seconds lease_ttl{120};
  std::chrono::seconds in_flight_grace{120};

  // Code intelligence (design section 6.5): the available language back
  // ends. When one matches the plan's target language, the run keeps a
  // symbol index at runs/<run>/codeintel.db in step with the integration
  // branch and the role MCP servers get it via --codeintel-db. Null (or no
  // back end for the language) disables indexing.
  codeintel::BackendRegistry* codeintel_backends = nullptr;

  // GitHub flow (design section 10.4.4); disabled without a transport. The
  // caller owns the transport (production passes a CprTransport).
  llm::HttpTransport* github_transport = nullptr;
  std::string github_token;
  // Optional: refreshed per request (GitHub App installation tokens, design
  // section 10.4.2); overrides github_token when set.
  std::function<Result<std::string>()> github_token_provider;
  std::string github_api_base = "https://api.github.com";
  std::string github_owner;
  std::string github_repo;
  std::string github_remote = "origin";
  // When the flow pushes (design sections 10.4.4, 15): milestone_done,
  // run_done and/or escalation. Empty disables every push.
  std::vector<std::string> github_push_on = {"milestone_done", "run_done",
                                             "escalation"};
  // Pre-push policy checked before every push (design section 10.4.6).
  github::PrePushConfig github_pre_push;
  // Pull-request policy (design sections 10.4.4, 10.4.8).
  bool github_pr_draft_until_done = true;
  std::vector<std::string> github_pr_labels;
  bool github_pr_request_codeowners = true;
  // Export defects to GitHub Issues alongside the PR flow (design 6.10).
  bool export_defects = true;
};

class RunProcess {
 public:
  RunProcess(store::RunStore& store, plan::Plan plan, RunProcessConfig config,
             git::Repository& repository, process::ProcessExecutor& executor);

  // Tests inject a driver factory; without one the production factory spawns
  // opencode servers per (role, worktree).
  void set_session_driver_factory(SessionDriverFactory factory);

  Result<RunState> execute(store::UnixTime now);

 private:
  store::RunStore& store_;
  plan::Plan plan_;
  RunProcessConfig config_;
  git::Repository& repository_;
  process::ProcessExecutor& executor_;
  SessionDriverFactory driver_factory_;
};

}  // namespace cf::orch
