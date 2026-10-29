// cf-ns-agent: bridge a sandboxed opencode server to the host.
//
// host mode runs the opencode process in the daemon-free namespace sandbox
// and forwards the host loopback port plus the egress proxy through Unix
// sockets in the shared run directory. inner mode runs inside the namespace:
// it exposes the two Unix sockets as loopback TCP endpoints for opencode.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <functional>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cf/common/process.hpp"
#include "cf/sandbox/namespaces.hpp"

namespace {

using cf::process::ProcessRequest;
using cf::sandbox::NamespaceExecutor;
using cf::sandbox::NamespaceSandboxSpec;

struct Options {
  std::string mode;
  std::string worktree;
  std::string run_dir;
  std::string bridge_dir;
  std::string role;
  std::string inner_binary = "/cf/bin/cf-ns-agent";
  std::string mcp_source;
  std::string opencode_source;
  std::string inner_log;
  std::string docs_source;
  std::string reference_docs_source;
  std::string reference_versions_source;
  std::string designs_source;
  std::string patterns_source;
  std::vector<std::string> mounts;
  std::vector<std::string> path_prefixes;
  std::vector<std::string> environment;
  std::vector<std::string> write_paths;
  std::vector<std::string> write_files;
  std::string proxy_url;
  std::string inner_proxy;
  uint16_t host_port = 0;
  uint16_t opencode_port = 0;
  std::vector<std::string> command;
};

void usage() {
  std::cerr
      << "usage: cf-ns-agent host --worktree DIR --run-dir DIR --bridge-dir DIR "
         "--role ROLE --host-port PORT --proxy HOST:PORT --mcp-source PATH -- "
         "opencode serve ...\n"
         "       cf-ns-agent inner --bridge-dir DIR --opencode-port PORT -- "
         "opencode serve ...\n";
}

bool parse_port(const std::string& value, uint16_t* port) {
  try {
    const unsigned long parsed = std::stoul(value);
    if (parsed == 0 || parsed > 65535) {
      return false;
    }
    *port = static_cast<uint16_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_proxy(std::string value, std::string* host, uint16_t* port) {
  const std::string scheme = "http://";
  if (value.rfind(scheme, 0) == 0) {
    value.erase(0, scheme.size());
  }
  const auto colon = value.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  *host = value.substr(0, colon);
  return parse_port(value.substr(colon + 1), port);
}

bool parse_options(int argc, char** argv, Options* options) {
  if (argc < 3) {
    return false;
  }
  options->mode = argv[1];
  int index = 2;
  const auto take = [&](const char* flag, std::string* value) {
    if (std::strcmp(argv[index], flag) != 0 || index + 1 >= argc) {
      return false;
    }
    *value = argv[++index];
    ++index;
    return true;
  };
  while (index < argc) {
    if (std::strcmp(argv[index], "--") == 0) {
      ++index;
      for (; index < argc; ++index) {
        options->command.push_back(argv[index]);
      }
      return !options->command.empty();
    }
    if (options->mode == "host") {
      if (std::strcmp(argv[index], "--worktree") == 0) {
        if (!take("--worktree", &options->worktree)) return false;
      } else if (std::strcmp(argv[index], "--run-dir") == 0) {
        if (!take("--run-dir", &options->run_dir)) return false;
      } else if (std::strcmp(argv[index], "--bridge-dir") == 0) {
        if (!take("--bridge-dir", &options->bridge_dir)) return false;
      } else if (std::strcmp(argv[index], "--role") == 0) {
        if (!take("--role", &options->role)) return false;
      } else if (std::strcmp(argv[index], "--proxy") == 0) {
        if (!take("--proxy", &options->proxy_url)) return false;
      } else if (std::strcmp(argv[index], "--inner-binary") == 0) {
        if (!take("--inner-binary", &options->inner_binary)) return false;
      } else if (std::strcmp(argv[index], "--mcp-source") == 0) {
        if (!take("--mcp-source", &options->mcp_source)) return false;
      } else if (std::strcmp(argv[index], "--opencode-source") == 0) {
        if (!take("--opencode-source", &options->opencode_source)) return false;
      } else if (std::strcmp(argv[index], "--log") == 0) {
        if (!take("--log", &options->inner_log)) return false;
      } else if (std::strcmp(argv[index], "--docs-source") == 0) {
        if (!take("--docs-source", &options->docs_source)) return false;
      } else if (std::strcmp(argv[index], "--reference-docs-source") == 0) {
        if (!take("--reference-docs-source", &options->reference_docs_source)) return false;
      } else if (std::strcmp(argv[index], "--reference-versions-source") == 0) {
        if (!take("--reference-versions-source", &options->reference_versions_source)) return false;
      } else if (std::strcmp(argv[index], "--designs-source") == 0) {
        if (!take("--designs-source", &options->designs_source)) return false;
      } else if (std::strcmp(argv[index], "--patterns-source") == 0) {
        if (!take("--patterns-source", &options->patterns_source)) return false;
      } else if (std::strcmp(argv[index], "--mount") == 0) {
        std::string mount;
        if (!take("--mount", &mount)) return false;
        options->mounts.push_back(std::move(mount));
      } else if (std::strcmp(argv[index], "--path") == 0) {
        std::string path;
        if (!take("--path", &path)) return false;
        options->path_prefixes.push_back(std::move(path));
      } else if (std::strcmp(argv[index], "--env") == 0) {
        std::string entry;
        if (!take("--env", &entry)) return false;
        options->environment.push_back(std::move(entry));
      } else if (std::strcmp(argv[index], "--write") == 0) {
        std::string entry;
        if (!take("--write", &entry)) return false;
        options->write_paths.push_back(std::move(entry));
      } else if (std::strcmp(argv[index], "--write-file") == 0) {
        std::string entry;
        if (!take("--write-file", &entry)) return false;
        options->write_files.push_back(std::move(entry));
      } else if (std::strcmp(argv[index], "--host-port") == 0) {
        std::string value;
        if (!take("--host-port", &value) || !parse_port(value, &options->host_port)) {
          return false;
        }
      } else {
        return false;
      }
    } else if (options->mode == "inner") {
      if (std::strcmp(argv[index], "--bridge-dir") == 0) {
        if (!take("--bridge-dir", &options->bridge_dir)) return false;
      } else if (std::strcmp(argv[index], "--opencode-port") == 0) {
        std::string value;
        if (!take("--opencode-port", &value) ||
            !parse_port(value, &options->opencode_port)) {
          return false;
        }
      } else if (std::strcmp(argv[index], "--proxy-socket") == 0) {
        if (!take("--proxy-socket", &options->inner_proxy)) return false;
      } else if (std::strcmp(argv[index], "--log") == 0) {
        if (!take("--log", &options->inner_log)) return false;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return false;
}

void relay(int first, int second) {
  pollfd fds[2] = {{first, POLLIN, 0}, {second, POLLIN, 0}};
  char buffer[16384];
  for (;;) {
    const int ready = ::poll(fds, 2, -1);
    if (ready <= 0) {
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    for (int i = 0; i < 2; ++i) {
      if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      const int target = i == 0 ? second : first;
      const ssize_t received = ::recv(fds[i].fd, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        ::shutdown(target, SHUT_WR);
        fds[i].fd = -1;
        continue;
      }
      ssize_t sent = 0;
      while (sent < received) {
        const ssize_t written = ::send(target, buffer + sent,
                                       static_cast<std::size_t>(received - sent),
                                       MSG_NOSIGNAL);
        if (written <= 0) {
          if (errno == EINTR) continue;
          ::shutdown(fds[i].fd, SHUT_RD);
          fds[i].fd = -1;
          break;
        }
        sent += written;
      }
    }
    if (fds[0].fd < 0 && fds[1].fd < 0) {
      break;
    }
  }
  ::close(first);
  ::close(second);
}

bool make_dir(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

int tcp_listener(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(fd, 64) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int unix_listener(const std::string& path) {
  ::unlink(path.c_str());
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(fd, 64) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int connect_unix(const std::string& path, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() < sizeof(address.sun_path)) {
      std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        return fd;
      }
    }
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return -1;
}

int connect_tcp_retry(const std::string& host, uint16_t port, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int fd = -1;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) == 0) {
      for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
        fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC,
                      entry->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
          break;
        }
        ::close(fd);
        fd = -1;
      }
      ::freeaddrinfo(result);
    }
    if (fd >= 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return -1;
}

int connect_tcp(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
    return -1;
  }
  int fd = -1;
  for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
    fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC,
                  entry->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(result);
  return fd;
}

void accept_loop(int listener, const std::function<void(int)>& handler) {
  for (;;) {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      return;
    }
    const int flags = ::fcntl(client, F_GETFD, 0);
    if (flags >= 0) {
      ::fcntl(client, F_SETFD, flags | FD_CLOEXEC);
    }
    std::thread(handler, client).detach();
  }
}

std::uint16_t local_port(int listener) {
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

std::string opencode_port(const std::vector<std::string>& command) {
  for (std::size_t i = 0; i + 1 < command.size(); ++i) {
    if (command[i] == "--port") {
      return command[i + 1];
    }
  }
  return {};
}

bool wait_for_child(pid_t child) {
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool install_binary(const std::filesystem::path& source,
                    const std::filesystem::path& target,
                    std::filesystem::perms permissions) {
  std::error_code ec;
  const std::filesystem::path temporary =
      target.string() + ".tmp." + std::to_string(::getpid());
  std::filesystem::copy_file(source, temporary,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    return false;
  }
  std::filesystem::permissions(temporary, permissions,
                               std::filesystem::perm_options::replace, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  return true;
}

std::atomic<pid_t> g_supervisor{0};
std::atomic<NamespaceExecutor*> g_executor{nullptr};

void terminate_supervisor(int) {
  const pid_t supervisor = g_supervisor.load();
  if (supervisor > 0) {
    ::kill(supervisor, SIGTERM);
  }
  ::_exit(0);
}

void stop_executor(int) {
  NamespaceExecutor* executor = g_executor.load();
  if (executor != nullptr) {
    executor->stop();
  }
  ::_exit(0);
}

int run_host(const Options& options) {
  std::string proxy_host;
  uint16_t proxy_port = 0;
  if (!parse_proxy(options.proxy_url, &proxy_host, &proxy_port)) {
    std::cerr << "cf-ns-agent: invalid or missing --proxy\n";
    return 2;
  }
  const std::string port_text = opencode_port(options.command);
  uint16_t opencode = 0;
  if (!parse_port(port_text, &opencode)) {
    std::cerr << "cf-ns-agent: opencode command needs --port\n";
    return 2;
  }
  if (!make_dir(options.bridge_dir) || !make_dir(options.run_dir + "/bin")) {
    std::cerr << "cf-ns-agent: cannot create the bridge directory\n";
    return 2;
  }
  const std::filesystem::path inner = options.run_dir + "/bin/cf-ns-agent";
  if (!install_binary("/proc/self/exe", inner,
                      std::filesystem::perms::owner_all |
                          std::filesystem::perms::group_read |
                          std::filesystem::perms::others_read)) {
    std::cerr << "cf-ns-agent: cannot install the inner helper\n";
    return 2;
  }

  for (const auto& file : options.write_files) {
    const std::filesystem::path path =
        std::filesystem::path(options.worktree) / file;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      std::filesystem::create_directories(path.parent_path(), ec);
      std::ofstream out(path, std::ios::binary);
      if (!out) {
        std::cerr << "cf-ns-agent: cannot create write-set file " << path << "\n";
        return 2;
      }
    }
  }

  const int host_listener = tcp_listener(opencode);
  const int egress_listener = unix_listener(options.bridge_dir + "/e.sock");
  if (host_listener < 0 || egress_listener < 0) {
    std::cerr << "cf-ns-agent: cannot bind the host bridge\n";
    return 2;
  }

  NamespaceSandboxSpec spec;
  spec.worktree = options.worktree;
  if (!options.write_paths.empty()) {
    spec.writable_paths = options.write_paths;
  } else if (options.role == "coder") {
    spec.writable_paths = {"src", "include"};
  } else if (options.role == "qa") {
    spec.writable_paths = {"tests", "qa"};
  }
  if (options.role == "coder" || options.role == "qa") {
    for (const char* artifact :
         {"target", "build", "dist", "node_modules", ".gradle", ".cache"}) {
      if (std::find(spec.writable_paths.begin(), spec.writable_paths.end(),
                    artifact) == spec.writable_paths.end()) {
        spec.writable_paths.emplace_back(artifact);
      }
    }
  }
  spec.extra_writable_mounts.push_back({options.run_dir, "/cf"});
  if (!options.mcp_source.empty()) {
    std::error_code ec;
    const auto source = std::filesystem::absolute(options.mcp_source, ec);
    if (ec) {
      std::cerr << "cf-ns-agent: cannot resolve cf-mcp\n";
      return 2;
    }
    spec.extra_readonly_mounts.push_back({source.string(), "/cf/bin/cf-mcp"});
  }
  if (!options.opencode_source.empty()) {
    std::error_code ec;
    const auto source = std::filesystem::absolute(options.opencode_source, ec);
    if (ec) {
      std::cerr << "cf-ns-agent: cannot resolve opencode\n";
      return 2;
    }
    spec.extra_readonly_mounts.push_back({source.string(), "/cf/bin/opencode"});
  }
  const auto mount_source = [&spec](const std::string& source, const char* target) {
    if (source.empty()) {
      return;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(source, ec);
    if (!ec) {
      spec.extra_readonly_mounts.push_back({absolute.string(), target});
    }
  };
  mount_source(options.docs_source, "/cf/docs");
  mount_source(options.reference_docs_source, "/cf/reference-docs");
  mount_source(options.reference_versions_source, "/cf/reference-versions.json");
  mount_source(options.designs_source, "/cf/designs");
  mount_source(options.patterns_source, "/cf/catalog");
  for (const auto& mount : options.mounts) {
    const auto first = mount.find(':');
    if (first == std::string::npos || first == 0 || first + 1 >= mount.size()) {
      std::cerr << "cf-ns-agent: invalid --mount (expected HOST:TARGET[:ro])\n";
      return 2;
    }
    const auto second = mount.find(':', first + 1);
    const std::string source = mount.substr(0, first);
    const std::string target =
        mount.substr(first + 1, second == std::string::npos
                                    ? std::string::npos
                                    : second - first - 1);
    if (target.empty() || target.front() != '/') {
      std::cerr << "cf-ns-agent: --mount target must be absolute\n";
      return 2;
    }
    if (second != std::string::npos && mount.substr(second + 1) != "ro") {
      std::cerr << "cf-ns-agent: only read-only --mount is supported\n";
      return 2;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(source, ec);
    if (ec) {
      std::cerr << "cf-ns-agent: cannot resolve mount source " << source << "\n";
      return 2;
    }
    spec.extra_readonly_mounts.push_back({absolute.string(), target});
  }
  spec.container_workdir = "/work";

  ProcessRequest request;
  request.argv = {options.inner_binary, "inner", "--bridge-dir", "/cf/bridge",
                  "--opencode-port", std::to_string(opencode)};
  if (!options.inner_log.empty()) {
    request.argv.push_back("--log");
    request.argv.push_back(options.inner_log);
  }
  request.argv.push_back("--");
  request.argv.insert(request.argv.end(), options.command.begin(),
                      options.command.end());
  request.working_directory = "/work";
  if (!options.path_prefixes.empty()) {
    std::string path;
    for (const auto& prefix : options.path_prefixes) {
      if (!path.empty()) {
        path += ':';
      }
      path += prefix;
    }
    const char* existing = std::getenv("PATH");
    if (existing != nullptr && *existing != '\0') {
      path += ':';
      path += existing;
    }
    request.environment.push_back({"PATH", path});
  }
  for (const auto& entry : options.environment) {
    const auto equals = entry.find('=');
    if (equals == std::string::npos || equals == 0) {
      std::cerr << "cf-ns-agent: invalid --env (expected KEY=VALUE)\n";
      return 2;
    }
    request.environment.push_back(
        {entry.substr(0, equals), entry.substr(equals + 1)});
  }
  request.timeout = std::chrono::hours(24);

  const pid_t supervisor = ::fork();
  if (supervisor < 0) {
    std::cerr << "cf-ns-agent: fork failed\n";
    return 2;
  }
  if (supervisor == 0) {
    ::setpgid(0, 0);
    ::close(host_listener);
    ::close(egress_listener);
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    NamespaceExecutor executor(std::move(spec));
    g_executor.store(&executor);
    ::signal(SIGTERM, stop_executor);
    const auto result = executor.run(request);
    if (!result.stdout_text.empty()) {
      (void)::write(STDOUT_FILENO, result.stdout_text.data(),
                    result.stdout_text.size());
    }
    if (!result.stderr_text.empty()) {
      (void)::write(STDERR_FILENO, result.stderr_text.data(),
                    result.stderr_text.size());
    }
    if (!result.launched) {
      const std::string error = "cf-ns-agent: inner launch failed: " + result.error + "\n";
      (void)::write(STDERR_FILENO, error.data(), error.size());
    }
    ::_exit(result.launched && result.exit_code == 0 ? 0 : 1);
  }

  g_supervisor.store(supervisor);
  ::setpgid(supervisor, supervisor);
  ::signal(SIGTERM, terminate_supervisor);
  ::signal(SIGINT, terminate_supervisor);
  std::thread inbound([host_listener, bridge = options.bridge_dir] {
    accept_loop(host_listener, [bridge](int client) {
      const int inner = connect_unix(bridge + "/o.sock", 60000);
      if (inner < 0) {
        ::close(client);
        return;
      }
      relay(client, inner);
    });
  });
  std::thread egress([egress_listener, proxy_host, proxy_port] {
    accept_loop(egress_listener, [proxy_host, proxy_port](int client) {
      const int upstream = connect_tcp(proxy_host, proxy_port);
      if (upstream < 0) {
        ::close(client);
        return;
      }
      relay(client, upstream);
    });
  });
  inbound.detach();
  egress.detach();
  const bool ok = wait_for_child(supervisor);
  ::_exit(ok ? 0 : 1);
}

int run_inner(const Options& options) {
  if (!make_dir(options.bridge_dir)) {
    return 2;
  }
  const int bridge_listener = unix_listener(options.bridge_dir + "/o.sock");
  const int egress_listener = tcp_listener(0);
  if (bridge_listener < 0 || egress_listener < 0) {
    std::cerr << "cf-ns-agent: cannot create the inner bridge\n";
    return 2;
  }
  const uint16_t proxy_port = local_port(egress_listener);
  if (proxy_port == 0) {
    return 2;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return 2;
  }
  if (child == 0) {
    ::close(bridge_listener);
    ::close(egress_listener);
    if (!options.inner_log.empty()) {
      const int fd = ::open(options.inner_log.c_str(), O_CREAT | O_WRONLY | O_APPEND,
                            0644);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
          ::close(fd);
        }
      }
    }
    const std::string proxy = "http://127.0.0.1:" + std::to_string(proxy_port);
    ::setenv("HTTP_PROXY", proxy.c_str(), 1);
    ::setenv("HTTPS_PROXY", proxy.c_str(), 1);
    ::setenv("NO_PROXY", "localhost,127.0.0.1", 1);
    if (::chdir("/work") != 0) {
      ::_exit(127);
    }
    std::vector<char*> argv;
    for (const auto& argument : options.command) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }

  std::thread inbound([bridge_listener, port = options.opencode_port] {
    accept_loop(bridge_listener, [port](int client) {
      const int opencode = connect_tcp_retry("127.0.0.1", port, 30000);
      if (opencode < 0) {
        ::close(client);
        return;
      }
      relay(client, opencode);
    });
  });
  std::thread egress([egress_listener, bridge = options.bridge_dir] {
    accept_loop(egress_listener, [bridge](int client) {
      const int upstream = connect_unix(bridge + "/e.sock", 60000);
      if (upstream < 0) {
        ::close(client);
        return;
      }
      relay(client, upstream);
    });
  });
  inbound.detach();
  egress.detach();
  const bool ok = wait_for_child(child);
  ::_exit(ok ? 0 : 1);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    usage();
    return 2;
  }
  if (options.mode == "host") {
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) {
      return 1;
    }
    return run_host(options);
  }
  if (options.mode == "inner") {
    return run_inner(options);
  }
  usage();
  return 2;
}
