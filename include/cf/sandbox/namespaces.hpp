// cf-sandbox: the daemon-free namespace executor (design section 6.8).
//
// Runs a command inside unprivileged user, mount, network, UTS, IPC and PID
// namespaces — no Docker, no root. The command becomes PID 1 in its own
// process tree with a private /proc; the mount layout mirrors the Docker
// sandbox (host toolchain read-only, the worktree at /work with the role's
// writable directories layered in, noexec tmpfs for /tmp and $HOME, an
// executable /build). Egress is blocked by the kernel (fresh network
// namespace). On top of the namespaces, Landlock confines file access to the
// sandbox layout and a seccomp filter removes dangerous syscalls; resource
// limits come from a delegated cgroup when one is available.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "cf/common/process.hpp"

namespace cf::sandbox {

struct NamespaceSandboxSpec {
  std::string worktree;                     // mounted read-only at /work
  std::vector<std::string> writable_paths;  // relative to the worktree
  // Additional read-only bind mounts, e.g. a dependency cache.
  std::vector<std::pair<std::string, std::string>> extra_readonly_mounts;
  std::string container_workdir = "/work";
  std::size_t tmp_mb = 64;
  std::size_t build_mb = 1024;
  // Best-effort cgroup v2 limits; when set, the run fails if no delegated
  // cgroup is available instead of silently running unbounded.
  std::size_t memory_mb = 0;
  std::size_t pids_limit = 0;
};

// True when the current cgroup v2 delegation allows creating child cgroups
// (systemd user delegation); cgroup limits are only enforceable then.
bool cgroup_delegated();

class NamespaceExecutor final : public process::ProcessExecutor {
 public:
  explicit NamespaceExecutor(NamespaceSandboxSpec spec);

  process::ProcessResult run(const process::ProcessRequest& request) override;

  const NamespaceSandboxSpec& spec() const { return spec_; }

 private:
  NamespaceSandboxSpec spec_;
};

}  // namespace cf::sandbox
