// cf-orch: the opencode server process (design sections 5.3, 6.6).
//
// Spawns `opencode serve` for one role with its generated config, waits until
// the health endpoint answers, and stops it (SIGTERM, then SIGKILL) on
// destruction. stdout/stderr go to a per-role log file.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

#include "cf/agents/opencode_client.hpp"
#include "cf/common/result.hpp"

namespace cf::orch {

struct OpencodeServerOptions {
  std::string binary = "opencode";
  // Optional command prefix (e.g. `docker run … <image>`): the server is
  // spawned as launcher + {binary, serve, --hostname, …, --port, …}. Empty
  // runs opencode directly on the host.
  std::vector<std::string> launcher;
  std::string hostname = "127.0.0.1";
  uint16_t port = 0;  // required: use find_free_port()
  std::filesystem::path config_path;        // opencode.json
  std::filesystem::path working_directory;
  std::filesystem::path log_path;           // stdout/stderr destination
  std::chrono::milliseconds start_timeout{30000};
  std::chrono::milliseconds stop_timeout{10000};
  std::vector<std::pair<std::string, std::string>> environment;
  // Sandboxed servers: the container name; stop() force-removes it so a
  // killed docker CLI can never leave a container behind.
  std::string cleanup_container;
};

class OpencodeServerProcess {
 public:
  // Spawns the server without waiting for health; the caller decides how to
  // reach it (for internal sandbox networks the container IP must be
  // discovered first).
  static Result<std::unique_ptr<OpencodeServerProcess>> spawn(
      OpencodeServerOptions options);

  // Polls /global/health through `client` until it answers or the start
  // timeout expires. On failure the process is killed and an error returned.
  Status wait_healthy(agents::OpencodeClient& client) const;

  // Spawns the server and waits for health through `client`.
  static Result<std::unique_ptr<OpencodeServerProcess>> start(
      OpencodeServerOptions options, agents::OpencodeClient& client);

  ~OpencodeServerProcess();
  OpencodeServerProcess(const OpencodeServerProcess&) = delete;
  OpencodeServerProcess& operator=(const OpencodeServerProcess&) = delete;

  void stop();
  bool running() const;
  uint16_t port() const { return options_.port; }
  pid_t pid() const { return pid_; }
  const OpencodeServerOptions& options() const { return options_; }

 private:
  OpencodeServerProcess(OpencodeServerOptions options, pid_t pid);

  OpencodeServerOptions options_;
  pid_t pid_ = -1;
};

// A free TCP port on the loopback interface.
uint16_t find_free_port();

}  // namespace cf::orch
