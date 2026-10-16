// cf-sandbox: Docker-based sandbox executor (design section 6.8).
//
// Implements the design's mount layout and defences with Docker primitives:
// a read-only worktree with writable role mounts, a read-only root filesystem,
// noexec tmpfs for /tmp and $HOME, an executable /build tmpfs, no network by
// default, all capabilities dropped, no-new-privileges, cgroup limits and the
// profile's toolchain image. The executor implements the same
// process::ProcessExecutor interface as the local runner, so the gate runner
// and the workspace driver are unchanged.
//
// The daemon-free namespace/Landlock/seccomp executor (also design section
// 6.8) can follow behind the same interface.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cf/common/process.hpp"

namespace cf::sandbox {

struct SandboxMount {
  std::string source;  // host path
  std::string target;  // container path
  bool writable = false;
};

struct SandboxSpec {
  std::string image;                         // the profile's toolchain image
  std::string worktree;                      // mounted read-only at /work
  std::vector<std::string> writable_paths;   // relative to the worktree
  std::vector<SandboxMount> extra_mounts;    // e.g. /deps dependency caches
  std::string workdir = "/work";
  bool network = false;         // false: --network none
  std::string network_name;     // docker network name when network is true
  std::string user;             // empty: the invoking user's uid:gid
  double cpus = 0.0;            // 0: no limit
  uint64_t memory_mb = 0;       // 0: no limit
  uint32_t pids_limit = 0;      // 0: no limit
  std::string docker_binary = "docker";
};

// Builds the `docker run` argument list (exposed for tests). An empty
// container_name omits --name.
std::vector<std::string> docker_run_argv(const SandboxSpec& spec,
                                         const process::ProcessRequest& request,
                                         const std::string& container_name = "");

class DockerSandboxExecutor final : public process::ProcessExecutor {
 public:
  explicit DockerSandboxExecutor(SandboxSpec spec);

  // Runs one command in a fresh container; on timeout the container is
  // force-removed so nothing is left behind.
  process::ProcessResult run(const process::ProcessRequest& request) override;

  const SandboxSpec& spec() const { return spec_; }

 private:
  SandboxSpec spec_;
};

// True when the docker CLI can talk to a daemon.
bool docker_available(const std::string& docker_binary = "docker");

}  // namespace cf::sandbox
