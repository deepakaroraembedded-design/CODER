#include "cf/sandbox/docker.hpp"

#include <atomic>
#include <chrono>
#include <sstream>
#include <utility>

#include <unistd.h>

namespace cf::sandbox {
namespace {

void add_option(std::vector<std::string>& argv, const std::string& name,
                const std::string& value) {
  argv.push_back(name);
  argv.push_back(value);
}

}  // namespace

std::vector<std::string> docker_run_argv(const SandboxSpec& spec,
                                         const process::ProcessRequest& request,
                                         const std::string& container_name) {
  std::vector<std::string> argv = {spec.docker_binary, "run", "--rm", "--read-only"};
  if (!container_name.empty()) {
    add_option(argv, "--name", container_name);
  }
  if (!spec.run_id.empty()) {
    add_option(argv, "--label", "cf.run=" + spec.run_id);
    add_option(argv, "--label", "cf.kind=gate");
  }
  if (spec.network) {
    add_option(argv, "--network", spec.network_name.empty() ? "bridge" : spec.network_name);
  } else {
    add_option(argv, "--network", "none");
  }
  add_option(argv, "--cap-drop", "ALL");
  add_option(argv, "--security-opt", "no-new-privileges");
  const std::string user = spec.user.empty()
                               ? std::to_string(::getuid()) + ":" + std::to_string(::getgid())
                               : spec.user;
  add_option(argv, "--user", user);
  add_option(argv, "--workdir", spec.workdir);
  if (spec.cpus > 0.0) {
    add_option(argv, "--cpus", std::to_string(spec.cpus));
  }
  if (spec.memory_mb > 0) {
    add_option(argv, "--memory", std::to_string(spec.memory_mb) + "m");
  }
  if (spec.pids_limit > 0) {
    add_option(argv, "--pids-limit", std::to_string(spec.pids_limit));
  }

  // Mount layout (design section 6.8.1): the whole worktree read-only, the
  // role's writable directories layered over it, the git directory read-only.
  if (!spec.worktree.empty()) {
    add_option(argv, "-v", spec.worktree + ":" + spec.workdir + ":ro");
    for (const auto& path : spec.writable_paths) {
      add_option(argv, "-v",
                 spec.worktree + "/" + path + ":" + spec.workdir + "/" + path);
    }
    add_option(argv, "-v", spec.worktree + "/.git:" + spec.workdir + "/.git:ro");
  }
  for (const auto& mount : spec.extra_mounts) {
    add_option(argv, "-v", mount.source + ":" + mount.target +
                               (mount.writable ? "" : ":ro"));
  }

  // Fresh tmpfs for /tmp and $HOME (noexec), executable /build for test
  // binaries and generated helpers (design sections 6.8.1, 6.8.2).
  add_option(argv, "--tmpfs", "/tmp:rw,noexec,nosuid,nodev,size=64m");
  add_option(argv, "--tmpfs", "/home/cf:rw,noexec,nosuid,nodev,size=64m");
  add_option(argv, "--tmpfs", "/build:rw,exec,nosuid,nodev,size=1g");
  add_option(argv, "-e", "HOME=/home/cf");

  for (const auto& [key, value] : request.environment) {
    add_option(argv, "-e", key + "=" + value);
  }

  argv.push_back(spec.image);
  argv.insert(argv.end(), request.argv.begin(), request.argv.end());
  return argv;
}

DockerSandboxExecutor::DockerSandboxExecutor(SandboxSpec spec)
    : spec_(std::move(spec)) {}

process::ProcessResult DockerSandboxExecutor::run(const process::ProcessRequest& request) {
  static std::atomic<uint64_t> counter{0};
  const std::string name = "cf-sandbox-" + std::to_string(::getpid()) + "-" +
                           std::to_string(counter.fetch_add(1));

  process::LocalProcessExecutor local;
  process::ProcessRequest docker_request;
  docker_request.argv = docker_run_argv(spec_, request, name);
  docker_request.timeout = request.timeout;
  docker_request.output_limit_bytes = request.output_limit_bytes;
  process::ProcessResult result = local.run(docker_request);

  if (result.timed_out) {
    // The docker CLI was killed; the container keeps running, so remove it.
    process::ProcessRequest cleanup;
    cleanup.argv = {spec_.docker_binary, "rm", "-f", name};
    cleanup.timeout = std::chrono::milliseconds(10000);
    (void)local.run(cleanup);
  }
  return result;
}

bool docker_available(const std::string& docker_binary) {
  process::LocalProcessExecutor local;
  process::ProcessRequest request;
  request.argv = {docker_binary, "info", "--format", "{{.ServerVersion}}"};
  request.timeout = std::chrono::milliseconds(10000);
  const process::ProcessResult result = local.run(request);
  return result.launched && result.exit_code == 0;
}

Result<std::size_t> remove_run_containers(const std::string& docker_binary,
                                          std::string_view run_id,
                                          std::chrono::milliseconds timeout) {
  process::LocalProcessExecutor local;

  process::ProcessRequest list;
  list.argv = {docker_binary, "ps", "-aq", "--filter",
               "label=cf.run=" + std::string(run_id)};
  list.timeout = timeout;
  const process::ProcessResult listed = local.run(list);
  if (!listed.launched) {
    return fail("SANDBOX_DOCKER_UNAVAILABLE",
                "cannot launch " + docker_binary + ": " + listed.error);
  }
  if (listed.exit_code != 0) {
    return fail("SANDBOX_DOCKER_FAILED",
                "docker ps failed: " + listed.stderr_text);
  }

  std::vector<std::string> containers;
  std::istringstream lines(listed.stdout_text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      containers.push_back(std::move(line));
    }
  }
  if (containers.empty()) {
    return 0;
  }

  process::ProcessRequest remove;
  remove.argv = {docker_binary, "rm", "-f"};
  remove.argv.insert(remove.argv.end(), containers.begin(), containers.end());
  remove.timeout = timeout;
  const process::ProcessResult removed = local.run(remove);
  if (!removed.launched) {
    return fail("SANDBOX_DOCKER_UNAVAILABLE",
                "cannot launch " + docker_binary + ": " + removed.error);
  }
  if (removed.exit_code != 0) {
    return fail("SANDBOX_DOCKER_FAILED",
                "docker rm failed: " + removed.stderr_text);
  }
  return containers.size();
}

}  // namespace cf::sandbox
