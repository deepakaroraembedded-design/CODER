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
#include <string>
#include <utility>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/git/repository.hpp"
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
  std::vector<std::pair<std::string, std::string>> environment;
  std::chrono::milliseconds turn_timeout{600000};

  // Sandbox the opencode servers (design section 6.8); empty runs them on the
  // host. The container needs a network for the model endpoint.
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
  std::string sandbox_image;
  std::chrono::milliseconds step_timeout{600000};

  // orchestration
  OrchestratorConfig orchestrator;
  uint32_t max_system_validation_attempts = 1;

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
