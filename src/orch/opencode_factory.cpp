#include "cf/orch/opencode_factory.hpp"

#include <chrono>
#include <fstream>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>
#include <unistd.h>

#include "cf/common/process.hpp"
#include "cf/orch/role_config.hpp"

namespace cf::orch {
namespace {

void add_option(std::vector<std::string>& argv, const std::string& name,
                const std::string& value) {
  argv.push_back(name);
  argv.push_back(value);
}

// The directories a role may write (relative to its worktree).
std::vector<std::string> writable_paths_for(const std::string& role) {
  if (role == "coder") {
    return {"src", "include"};
  }
  if (role == "qa") {
    return {"tests", "qa"};
  }
  return {};  // the reviewer is read-only
}

// Runs a docker command and returns trimmed stdout on success.
Result<std::string> docker_command(const std::string& docker_binary,
                                   const std::vector<std::string>& args) {
  process::LocalProcessExecutor local;
  process::ProcessRequest request;
  request.argv.push_back(docker_binary);
  request.argv.insert(request.argv.end(), args.begin(), args.end());
  request.timeout = std::chrono::milliseconds(30000);
  const process::ProcessResult result = local.run(request);
  if (!result.launched) {
    return fail("ORCH_DOCKER_FAILED", "cannot run docker: " + result.error);
  }
  if (result.exit_code != 0) {
    return fail("ORCH_DOCKER_FAILED",
                "docker " + (args.empty() ? std::string() : args.front()) +
                    " failed: " + result.stderr_text);
  }
  std::string text = result.stdout_text;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

struct SandboxNetwork {
  std::string name;
  bool internal = false;
  std::string gateway;  // the host address on the bridge
};

// Ensures the sandbox network exists (creating an internal one when missing)
// and reports whether it blocks egress (design section 6.8.2).
Result<SandboxNetwork> ensure_sandbox_network(const std::string& docker_binary,
                                              const std::string& name) {
  SandboxNetwork network;
  network.name = name;
  auto inspected = docker_command(
      docker_binary, {"network", "inspect", "-f", "{{.Internal}}", name});
  if (!inspected) {
    auto created = docker_command(docker_binary,
                                  {"network", "create", "--internal", name});
    if (!created) {
      return tl::make_unexpected(created.error());
    }
    network.internal = true;
  } else {
    network.internal = *inspected == "true";
  }
  if (network.internal) {
    auto gateway = docker_command(
        docker_binary,
        {"network", "inspect", "-f", "{{(index .IPAM.Config 0).Gateway}}", name});
    if (!gateway) {
      return tl::make_unexpected(gateway.error());
    }
    network.gateway = *gateway;
  }
  return network;
}

// The container's IP on its sandbox network (internal networks cannot publish
// ports, so the orchestrator connects to the IP directly).
Result<std::string> container_ip(const std::string& docker_binary,
                                 const std::string& container) {
  return docker_command(docker_binary,
                        {"inspect", "-f",
                         "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}",
                         container});
}

// Waits until the container has an address on the sandbox network.
Result<std::string> wait_for_container_ip(const std::string& docker_binary,
                                          const std::string& container,
                                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto ip = container_ip(docker_binary, container);
    if (ip.has_value() && !ip->empty()) {
      return ip;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return fail("ORCH_CONTAINER_NO_IP",
              "container " + container + " has no IP on the sandbox network");
}

}  // namespace

std::string sandboxed_container_name(const std::string& role, uint16_t port) {
  return "cf-oc-" + role + "-" + std::to_string(port);
}

std::vector<std::string> sandboxed_server_launcher(
    const OpencodeFactoryConfig& config, const std::string& role,
    const std::string& worktree, uint16_t port,
    const std::filesystem::path& config_path,
    const std::filesystem::path& calls_file) {
  std::vector<std::string> argv = {config.docker_binary, "run", "--rm", "--read-only"};
  add_option(argv, "--name", sandboxed_container_name(role, port));
  if (!config.run_id.empty()) {
    // Recovery removes orphaned containers by run label (design 6.12.4).
    add_option(argv, "--label", "cf.run=" + config.run_id);
    add_option(argv, "--label", "cf.kind=agent");
  }
  if (!config.sandbox_internal_network) {
    // Port publishing needs a routable bridge; --network none cannot publish.
    add_option(argv, "-p", "127.0.0.1:" + std::to_string(port) + ":" +
                               std::to_string(port));
  }
  add_option(argv, "--network",
             config.sandbox_network.empty() ? "bridge" : config.sandbox_network);
  add_option(argv, "--cap-drop", "ALL");
  add_option(argv, "--security-opt", "no-new-privileges");
  add_option(argv, "--user",
             std::to_string(::getuid()) + ":" + std::to_string(::getgid()));
  add_option(argv, "--workdir", "/work");

  // Mount layout (design section 6.8.1).
  add_option(argv, "-v", worktree + ":/work:ro");
  const std::vector<std::string> writable = writable_paths_for(role);
  for (const auto& path : writable) {
    add_option(argv, "-v", worktree + "/" + path + ":/work/" + path);
  }
  add_option(argv, "-v", worktree + "/.git:/work/.git:ro");
  add_option(argv, "-v", config.run_dir.string() + ":/cf");
  if (!config.mcp_binary_host.empty()) {
    add_option(argv, "-v", config.mcp_binary_host + ":/usr/local/bin/cf-mcp:ro");
  }
  for (const auto& [source, target] : config.extra_mounts) {
    add_option(argv, "-v", source + ":" + target);
  }
  if (!config.designs_dir.empty()) {
    // Approved design packages (design section 6.2 step 7), read-only.
    add_option(argv, "-v", config.designs_dir + ":/cf/designs:ro");
  }
  if (!config.patterns_dir.empty()) {
    // Pattern catalog (design section 6.2), read-only.
    add_option(argv, "-v", config.patterns_dir + ":/cf/catalog:ro");
  }

  add_option(argv, "--tmpfs", "/tmp:rw,noexec,nosuid,nodev,size=64m");
  add_option(argv, "--tmpfs", "/home/cf:rw,noexec,nosuid,nodev,size=64m");
  add_option(argv, "--tmpfs", "/build:rw,exec,nosuid,nodev,size=1g");

  add_option(argv, "-e", "HOME=/home/cf");
  add_option(argv, "-e", "OPENCODE_CONFIG=/cf/" + config_path.filename().string());
  add_option(argv, "-e", "CF_MCP_CALLS_FILE=/cf/" + calls_file.filename().string());
  add_option(argv, "-e", "CF_ROLE=" + role);
  if (!config.sandbox_proxy_url.empty()) {
    // All egress goes through the allow-listing proxy (design 6.8.2).
    if (!config.sandbox_internal_network) {
      add_option(argv, "--add-host", "host.docker.internal:host-gateway");
    }
    add_option(argv, "-e", "HTTP_PROXY=" + config.sandbox_proxy_url);
    add_option(argv, "-e", "HTTPS_PROXY=" + config.sandbox_proxy_url);
    add_option(argv, "-e", "NO_PROXY=localhost,127.0.0.1");
  }
  for (const auto& [key, value] : config.environment) {
    add_option(argv, "-e", key + "=" + value);
  }
  argv.push_back(config.sandbox_image);
  return argv;
}

OpencodeRoleFactory::RoleState::~RoleState() {
  if (stream) {
    stream->stop();
  }
}

OpencodeRoleFactory::OpencodeRoleFactory(OpencodeFactoryConfig config)
    : config_(std::move(config)) {
  // Force lazy default-logger creation on this thread: the event-stream
  // threads log disconnects, and creating the logger concurrently with that
  // use is a data race (TSan).
  (void)spdlog::default_logger();
}

OpencodeRoleFactory::~OpencodeRoleFactory() = default;

Result<std::unique_ptr<SessionDriver>> OpencodeRoleFactory::create(
    const std::string& role, const std::string& worktree) {
  const std::string key = role + "|" + worktree;
  auto existing = roles_.find(key);
  if (existing != roles_.end()) {
    if (existing->second.server && !existing->second.server->running()) {
      // The server died mid-run: drop the state (stopping the dead process
      // and its stream) and start a fresh server for the same role and
      // worktree (design section 6.12, scenario H8).
      spdlog::warn("opencode[{}] server died; restarting it", role);
      roles_.erase(existing);
      existing = roles_.end();
    }
  }
  if (existing != roles_.end()) {
    OpencodeServerProcess* server = existing->second.server.get();
    return std::unique_ptr<SessionDriver>(std::make_unique<OpencodeSessionDriver>(
        *existing->second.client, existing->second.idle_tracker.get(),
        existing->second.stream.get(),
        [server] { return server == nullptr || server->running(); }));
  }

  // Start the egress allow-listing proxy once (design section 6.8.2). On an
  // internal network the proxy binds to the bridge gateway; the kernel
  // already blocks everything else, so the proxy only serves the model calls.
  if (!config_.sandbox_image.empty()) {
    const std::string network_name =
        config_.sandbox_network.empty() ? "cf-sandbox" : config_.sandbox_network;
    auto network = ensure_sandbox_network(config_.docker_binary, network_name);
    if (!network) {
      return tl::make_unexpected(network.error());
    }
    config_.sandbox_network = network->name;
    config_.sandbox_internal_network = network->internal;
    if (!config_.sandbox_allowed_hosts.empty() && proxy_ == nullptr) {
      cf::sandbox::ProxyOptions proxy_options;
      proxy_options.allowed_hosts = config_.sandbox_allowed_hosts;
      proxy_options.listen_host =
          network->internal ? network->gateway : "127.0.0.1";
      auto proxy = cf::sandbox::EgressProxy::start(proxy_options);
      if (!proxy) {
        return tl::make_unexpected(proxy.error());
      }
      proxy_ = std::move(*proxy);
      config_.sandbox_proxy_url =
          network->internal
              ? "http://" + network->gateway + ":" + std::to_string(proxy_->port())
              : "http://host.docker.internal:" + std::to_string(proxy_->port());
      spdlog::info("opencode egress proxy on {}:{} ({} allow-listed host "
                   "pattern(s))",
                   proxy_options.listen_host, proxy_->port(),
                   config_.sandbox_allowed_hosts.size());
    }
  }

  // 1. The role's config and calls file. Sandboxed servers see the run
  // directory at /cf, so every path in the config is a container path.
  const bool sandboxed = !config_.sandbox_image.empty();
  const std::filesystem::path calls_file =
      config_.run_dir / ("calls-" + role + ".jsonl");
  RoleConfigSpec spec;
  spec.role = role;
  spec.provider_base_url = config_.provider_base_url;
  spec.api_key_env = config_.api_key_env;
  const auto model = config_.role_models.find(role);
  if (model != config_.role_models.end()) {
    spec.provider_model_id = model->second;
  }
  const auto role_options = config_.role_options.find(role);
  if (role_options != config_.role_options.end()) {
    spec.provider_options = role_options->second;
  }
  spec.allowed_commands = role == "qa" ? config_.qa_commands : config_.coder_commands;
  if (role == "reviewer") {
    spec.allowed_commands.clear();
  }
  spec.mcp_command = sandboxed ? "/usr/local/bin/cf-mcp" : config_.mcp_command;
  spec.run_id = config_.run_id;
  spec.db_path = sandboxed ? "/cf/run.db" : config_.db_path;
  spec.docs_dir = sandboxed ? "/cf/docs" : config_.docs_dir;
  if (!config_.codeintel_db.empty()) {
    spec.codeintel_db = sandboxed ? "/cf/codeintel.db" : config_.codeintel_db;
  }
  spec.product = config_.product;
  spec.language = config_.language;
  if (!config_.designs_dir.empty()) {
    spec.designs_dir = sandboxed ? "/cf/designs" : config_.designs_dir;
  }
  if (!config_.patterns_dir.empty()) {
    spec.patterns_dir = sandboxed ? "/cf/catalog" : config_.patterns_dir;
  }
  spec.calls_file = sandboxed ? "/cf/" + calls_file.filename().string()
                              : calls_file.string();

  // The writable mount points must exist before the container starts,
  // otherwise Docker creates them root-owned and the agent cannot write.
  for (const auto& path : writable_paths_for(role)) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(worktree) / path, ec);
  }

  const std::filesystem::path config_path =
      config_.run_dir / ("opencode-" + role + ".json");
  {
    std::ofstream out(config_path, std::ios::binary);
    if (!out) {
      return fail("ORCH_CONFIG_WRITE_FAILED",
                  "cannot write " + config_path.string());
    }
    out << build_role_config(spec).dump(2) << "\n";
  }

  // 2. Spawn the server and wait for health.
  const uint16_t port = find_free_port();
  if (port == 0) {
    return fail("ORCH_INVALID_PORT", "cannot find a free port for opencode");
  }
  RoleState state;
  state.idle_tracker = std::make_unique<SessionIdleTracker>();
  agents::OpencodeClientOptions client_options;
  client_options.host = config_.hostname;
  client_options.port = port;

  OpencodeServerOptions server_options;
  server_options.port = port;
  server_options.config_path = config_path;
  server_options.working_directory = worktree;
  server_options.log_path = config_.run_dir / ("opencode-" + role + ".log");
  server_options.start_timeout = config_.start_timeout;
  if (sandboxed) {
    server_options.launcher =
        sandboxed_server_launcher(config_, role, worktree, port, config_path,
                                  calls_file);
    server_options.binary = config_.sandbox_binary;
    server_options.hostname = "0.0.0.0";  // reached from the host by IP
    server_options.cleanup_container = sandboxed_container_name(role, port);
    if (config_.sandbox_internal_network) {
      // No published port on an internal network: discover the container IP
      // and connect to it directly (the kernel blocks everything else).
      auto spawned = OpencodeServerProcess::spawn(server_options);
      if (!spawned) {
        return tl::make_unexpected(spawned.error());
      }
      state.server = std::move(*spawned);
      auto ip = wait_for_container_ip(config_.docker_binary,
                                      server_options.cleanup_container,
                                      server_options.start_timeout);
      if (!ip) {
        return tl::make_unexpected(ip.error());
      }
      client_options.host = *ip;
      state.client = std::make_unique<agents::OpencodeClient>(client_options);
      auto healthy = state.server->wait_healthy(*state.client);
      if (!healthy) {
        return tl::make_unexpected(healthy.error());
      }
    } else {
      state.client = std::make_unique<agents::OpencodeClient>(client_options);
      auto server = OpencodeServerProcess::start(server_options, *state.client);
      if (!server) {
        return tl::make_unexpected(server.error());
      }
      state.server = std::move(*server);
    }
  } else {
    server_options.binary = config_.opencode_binary;
    server_options.environment = config_.environment;
    server_options.environment.push_back({"CF_MCP_CALLS_FILE", calls_file.string()});
    server_options.environment.push_back({"CF_ROLE", role});
    state.client = std::make_unique<agents::OpencodeClient>(client_options);
    auto server = OpencodeServerProcess::start(server_options, *state.client);
    if (!server) {
      return tl::make_unexpected(server.error());
    }
    state.server = std::move(*server);
  }

  // 3. Event stream: permissions to the broker, questions to the policy. The
  // broker sees container paths for sandboxed servers, so its worktree root
  // is the container's /work.
  state.stream = state.client->open_event_stream();
  agents::PermissionPolicyConfig policy;
  policy.worktree = sandboxed ? "/work" : worktree;
  if (role == "coder") {
    policy.writable_paths = {"src", "include"};
  } else if (role == "qa") {
    policy.writable_paths = {"tests", "qa"};
  }
  policy.allowed_commands = role == "qa" ? config_.qa_commands : config_.coder_commands;
  if (role == "reviewer") {
    policy.allowed_commands.clear();
  }
  policy.deny_commands = {"git push", "git remote", "git config", "git reset",
                          "git add",  "git commit", "curl",     "wget",
                          "ssh",      "sudo"};
  agents::OpencodeClient* client = state.client.get();
  state.broker = std::make_unique<agents::PermissionBroker>(
      agents::PermissionPolicy(std::move(policy)),
      [client](const agents::PermissionRequest& request,
               const agents::PermissionDecision& decision) {
        (void)client->reply_permission(
            request.session_id, request.request_id,
            decision.reply == agents::PermissionReply::Once ? "once" : "reject",
            decision.reason);
      });
  EventRouterHooks hooks;
  if (config_.retriever != nullptr) {
    QuestionPolicyConfig question_config;
    question_config.ambiguities_file = config_.ambiguities_file;
    question_config.product = config_.product;
    question_config.language = config_.language;
    state.question_policy =
        std::make_unique<RagQuestionPolicy>(*config_.retriever, question_config);
  }
  RagQuestionPolicy* question_policy = state.question_policy.get();
  hooks.reply_question = [client, question_policy](const QuestionAsked& asked) {
    QuestionDecision decision;
    if (question_policy != nullptr) {
      decision = question_policy->decide(asked);
    } else {
      decision.answer = false;
      decision.reason =
          "questions are not answered in this build; continue with the task";
    }
    if (decision.answer) {
      (void)client->reply_question(asked.session_id, asked.request_id,
                                   decision.answers);
    } else {
      (void)client->reject_question(asked.session_id, asked.request_id);
    }
    return decision;
  };
  state.router =
      std::make_unique<OpencodeEventRouter>(role, *state.broker, std::move(hooks));
  if (state.stream) {
    OpencodeEventRouter* router = state.router.get();
    SessionIdleTracker* tracker = state.idle_tracker.get();
    state.stream->set_callback([router, tracker](const agents::OpencodeEvent& event) {
      tracker->record(event);
      (void)router->handle(event);
    });
    state.stream->set_disconnect_callback([role](const std::string& reason) {
      spdlog::warn("opencode[{}] event stream disconnected: {}", role, reason);
    });
  }

  OpencodeServerProcess* server = state.server.get();
  auto driver = std::unique_ptr<SessionDriver>(std::make_unique<OpencodeSessionDriver>(
      *state.client, state.idle_tracker.get(), state.stream.get(),
      [server] { return server == nullptr || server->running(); }));
  roles_.emplace(key, std::move(state));
  return driver;
}

}  // namespace cf::orch
