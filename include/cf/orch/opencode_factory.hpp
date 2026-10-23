// cf-orch: the production session-driver factory (design sections 5.3, 6.6).
//
// Per (role, worktree): writes the role's opencode.json, spawns
// `opencode serve`, opens the event stream, and wires permission and question
// events to the role's broker and policy. One instance owns every server it
// spawned and stops them on destruction.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cf/orch/event_router.hpp"
#include "cf/orch/opencode_server.hpp"
#include "cf/orch/opencode_session_driver.hpp"
#include "cf/orch/question_policy.hpp"
#include "cf/orch/session_agent_runner.hpp"
#include "cf/sandbox/proxy.hpp"

namespace cf::orch {

struct OpencodeFactoryConfig {
  std::string run_id;
  std::filesystem::path run_dir;  // <workspace>/runs/<run>
  std::string opencode_binary = "opencode";
  std::string hostname = "127.0.0.1";
  std::string provider_base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key_env = "FIREWORKS_API_KEY";
  std::map<std::string, std::string, std::less<>> role_models;
  // Extra opencode model options per role (see RoleConfigSpec).
  std::map<std::string, nlohmann::json, std::less<>> role_options;
  std::string mcp_command;  // path to the cf-mcp binary
  std::string db_path;      // run store
  std::string docs_dir;
  // The run's code intelligence index (design section 6.5); the role's
  // cf-mcp gets it via --codeintel-db. Empty: no index (no back end for the
  // plan's language).
  std::string codeintel_db;
  // RAG applicability context and approved design packages for the role's
  // cf-mcp (design sections 6.2, 6.3).
  std::string product;
  std::string language;
  std::string designs_dir;
  std::string patterns_dir;  // pattern catalog (design section 6.2)
  std::vector<std::string> coder_commands;  // profile agent commands
  std::vector<std::string> qa_commands;
  std::chrono::milliseconds start_timeout{30000};
  // RAG-backed question answering (design section 6.6.1 layer 3); without a
  // retriever, questions are rejected and recorded.
  rag::Retriever* retriever = nullptr;
  std::filesystem::path ambiguities_file;
  // Extra environment for the server processes (tests use it to point at a
  // fake opencode).
  std::vector<std::pair<std::string, std::string>> environment;

  // Sandbox (design section 6.8): when sandbox_image is set, each opencode
  // server runs in a container with the design's mount layout instead of on
  // the host. The container sees the worktree at /work and the run directory
  // at /cf (config, calls file, store, docs).
  std::string sandbox_image;
  std::string sandbox_binary = "opencode";  // program inside the container
  // Empty: the default bridge network. The server must be reachable through
  // its published port and needs a route to the model endpoint; strict
  // egress allow-listing is still pending (design section 6.8.2), so point
  // this at a user-defined network with restricted egress where available.
  std::string sandbox_network;
  std::string docker_binary = "docker";
  // Host path of the cf-mcp binary, mounted at /usr/local/bin/cf-mcp.
  std::string mcp_binary_host;
  std::vector<std::pair<std::string, std::string>> extra_mounts;  // host:container[:ro]
  // Egress allow-list (design section 6.8.2): when non-empty, the factory
  // starts a filtering CONNECT proxy and points the containers at it through
  // host.docker.internal; everything else is denied.
  std::vector<std::string> sandbox_allowed_hosts;
  // Set by the factory once the proxy is running.
  std::string sandbox_proxy_url;
  // Set by the factory: the sandbox network blocks all egress, so no port is
  // published and the orchestrator connects to the container IP directly.
  bool sandbox_internal_network = false;
};

// The container name for a sandboxed server (unique per role and port).
std::string sandboxed_container_name(const std::string& role, uint16_t port);

// Builds the `docker run` prefix for a sandboxed opencode server (exposed for
// tests): read-only root filesystem, the §6.8.1 mount layout, hardening
// flags, tmpfs and environment.
std::vector<std::string> sandboxed_server_launcher(
    const OpencodeFactoryConfig& config, const std::string& role,
    const std::string& worktree, uint16_t port,
    const std::filesystem::path& config_path, const std::filesystem::path& calls_file);

class OpencodeRoleFactory {
 public:
  explicit OpencodeRoleFactory(OpencodeFactoryConfig config);
  ~OpencodeRoleFactory();

  OpencodeRoleFactory(const OpencodeRoleFactory&) = delete;
  OpencodeRoleFactory& operator=(const OpencodeRoleFactory&) = delete;

  // Returns a driver for the role; the first call for a (role, worktree)
  // spawns and wires the server.
  Result<std::unique_ptr<SessionDriver>> create(const std::string& role,
                                                const std::string& worktree);

 private:
  struct RoleState {
    RoleState() = default;
    RoleState(RoleState&&) = default;
    RoleState& operator=(RoleState&&) = default;
    ~RoleState();
    std::unique_ptr<agents::OpencodeClient> client;
    std::unique_ptr<OpencodeServerProcess> server;
    std::unique_ptr<agents::EventStream> stream;
    std::unique_ptr<agents::PermissionBroker> broker;
    std::unique_ptr<OpencodeEventRouter> router;
    std::unique_ptr<RagQuestionPolicy> question_policy;
    // Turn completion (session.idle events) for this role's server.
    std::unique_ptr<SessionIdleTracker> idle_tracker;
  };

  OpencodeFactoryConfig config_;
  std::map<std::string, RoleState, std::less<>> roles_;
  std::unique_ptr<cf::sandbox::EgressProxy> proxy_;
};

}  // namespace cf::orch
