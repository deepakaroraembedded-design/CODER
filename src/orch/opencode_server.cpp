#include "cf/orch/opencode_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cf/common/process.hpp"

namespace cf::orch {
namespace {

std::string tail_of_log(const std::filesystem::path& path, std::size_t max_bytes = 2000) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  const std::streamoff begin =
      size > static_cast<std::streamoff>(max_bytes) ? size - max_bytes : 0;
  in.seekg(begin);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return text;
}

bool child_exited(pid_t pid, int* status) {
  const pid_t waited = ::waitpid(pid, status, WNOHANG);
  return waited == pid;
}

}  // namespace

uint16_t find_free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::close(fd);
    return 0;
  }
  ::close(fd);
  return ntohs(address.sin_port);
}

OpencodeServerProcess::OpencodeServerProcess(OpencodeServerOptions options, pid_t pid)
    : options_(std::move(options)), pid_(pid) {}

OpencodeServerProcess::~OpencodeServerProcess() { stop(); }

Result<std::unique_ptr<OpencodeServerProcess>> OpencodeServerProcess::spawn(
    OpencodeServerOptions options) {
  if (options.port == 0) {
    return fail("ORCH_INVALID_PORT", "opencode server needs an explicit port");
  }

  const pid_t parent = ::getpid();
  const pid_t pid = ::fork();
  if (pid < 0) {
    return fail("ORCH_SERVER_SPAWN_FAILED",
                std::string("fork: ") + std::strerror(errno));
  }
  if (pid == 0) {
    ::setsid();
    // Guardrail: the run process is the server's only intended owner. If it
    // dies - even by SIGKILL - the kernel must take the server down with it,
    // so no orphan `opencode serve` survives to be cleaned up by hand: the
    // user's own session is also named `opencode`, and a name-based kill
    // (`pkill opencode`, `kill $(pgrep -x opencode)`) would kill it too.
    // Skipped when a launcher (docker) owns the process: there the container,
    // not this pid, is the server.
    if (options.launcher.empty()) {
      (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
      if (::getppid() != parent) {
        ::_exit(127);  // the parent died between fork and prctl
      }
    }
    if (!options.log_path.empty()) {
      const int fd = ::open(options.log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND,
                            0644);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
          ::close(fd);
        }
      }
    }
    if (!options.working_directory.empty() &&
        ::chdir(options.working_directory.c_str()) != 0) {
      ::_exit(127);
    }
    for (const auto& [key, value] : options.environment) {
      ::setenv(key.c_str(), value.c_str(), 1);
    }
    std::vector<std::string> arguments = options.launcher;
    arguments.push_back(options.binary);
    arguments.push_back("serve");
    arguments.push_back("--hostname");
    arguments.push_back(options.hostname);
    arguments.push_back("--port");
    arguments.push_back(std::to_string(options.port));
    if (options.launcher.empty() && !options.config_path.empty()) {
      ::setenv("OPENCODE_CONFIG", options.config_path.c_str(), 1);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }

  return std::unique_ptr<OpencodeServerProcess>(
      new OpencodeServerProcess(std::move(options), pid));
}

Status OpencodeServerProcess::wait_healthy(agents::OpencodeClient& client) const {
  const auto deadline = std::chrono::steady_clock::now() + options_.start_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (pid_ > 0 && child_exited(pid_, &status)) {
      const std::string log = tail_of_log(options_.log_path);
      const_cast<OpencodeServerProcess*>(this)->pid_ = -1;  // already reaped
      return fail("ORCH_SERVER_EXITED",
                  "opencode serve exited before becoming healthy" +
                      (log.empty() ? std::string() : ": " + log));
    }
    // opencode accepts TCP connections seconds before its HTTP stack answers
    // (it is still initializing); bound each probe so the loop can retry
    // within the overall start timeout instead of blocking on one request.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    auto health = client.health(std::min(remaining, std::chrono::milliseconds(2000)));
    if (health.has_value()) {
      return {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return fail("ORCH_SERVER_START_TIMEOUT",
              "opencode serve did not answer /global/health within " +
                  std::to_string(options_.start_timeout.count()) + " ms" +
                  (tail_of_log(options_.log_path).empty()
                       ? std::string()
                       : ": " + tail_of_log(options_.log_path)));
}

Result<std::unique_ptr<OpencodeServerProcess>> OpencodeServerProcess::start(
    OpencodeServerOptions options, agents::OpencodeClient& client) {
  auto process = spawn(std::move(options));
  if (!process) {
    return process;
  }
  auto healthy = (*process)->wait_healthy(client);
  if (!healthy) {
    (*process)->stop();
    return tl::make_unexpected(healthy.error());
  }
  return process;
}

void OpencodeServerProcess::stop() {
  if (!options_.cleanup_container.empty()) {
    process::LocalProcessExecutor local;
    process::ProcessRequest cleanup;
    cleanup.argv = {options_.launcher.empty() ? "docker" : options_.launcher.front(),
                    "rm", "-f", options_.cleanup_container};
    cleanup.timeout = std::chrono::milliseconds(10000);
    (void)local.run(cleanup);
  }
  if (pid_ <= 0) {
    return;
  }
  ::kill(pid_, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + options_.stop_timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (child_exited(pid_, &status)) {
      pid_ = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::kill(pid_, SIGKILL);
  ::waitpid(pid_, &status, 0);
  pid_ = -1;
}

bool OpencodeServerProcess::running() const {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
  return waited != pid_;
}

}  // namespace cf::orch
