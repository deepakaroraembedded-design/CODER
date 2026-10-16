#include "cf/sandbox/proxy.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cf::sandbox {
namespace {

// Splits "host:port" (or a bare host) into host and port; 0 means any port.
void split_pattern(const std::string& pattern, std::string& host, uint16_t& port) {
  const std::size_t colon = pattern.rfind(':');
  if (colon != std::string::npos && pattern.find(':') == colon) {
    host = pattern.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(pattern.substr(colon + 1)));
  } else {
    host = pattern;
    port = 0;
  }
}

bool matches(const std::string& host_pattern, const std::string& host) {
  if (host_pattern.rfind("*.", 0) == 0) {
    const std::string suffix = host_pattern.substr(1);  // ".example.com"
    return host.size() > suffix.size() &&
           host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
  }
  return host_pattern == host;
}

bool send_all(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (written <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

int connect_to(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses) != 0) {
    return -1;
  }
  int fd = -1;
  for (addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(addresses);
  return fd;
}

}  // namespace

bool host_allowed(const std::vector<std::string>& patterns, std::string_view host,
                  uint16_t port) {
  for (const auto& pattern : patterns) {
    std::string pattern_host;
    uint16_t pattern_port = 0;
    split_pattern(pattern, pattern_host, pattern_port);
    if (pattern_port != 0 && pattern_port != port) {
      continue;
    }
    if (matches(pattern_host, std::string(host))) {
      return true;
    }
  }
  return false;
}

struct EgressProxy::Impl {
  ProxyOptions options;
  int listen_fd = -1;
  std::atomic<bool> running{false};
  std::thread accept_thread;
  std::atomic<std::size_t> allowed{0};
  std::atomic<std::size_t> denied{0};

  void serve(int client_fd);
  void accept_loop(int listen_fd);
  void shutdown();
};

void EgressProxy::Impl::serve(int client_fd) {
  // Read the request head (request line + headers).
  std::string request;
  char buffer[4096];
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.size() < 16384) {
    const ssize_t got = ::recv(client_fd, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      ::close(client_fd);
      return;
    }
    request.append(buffer, static_cast<std::size_t>(got));
  }

  const std::size_t line_end = request.find("\r\n");
  const std::string request_line =
      line_end == std::string::npos ? request : request.substr(0, line_end);
  const std::size_t space = request_line.find(' ');
  const std::size_t second_space =
      space == std::string::npos ? std::string::npos : request_line.find(' ', space + 1);
  const std::string method =
      space == std::string::npos ? request_line : request_line.substr(0, space);
  const std::string target =
      space == std::string::npos
          ? std::string()
          : request_line.substr(space + 1, second_space == std::string::npos
                                              ? std::string::npos
                                              : second_space - space - 1);

  if (method != "CONNECT") {
    ++denied;
    (void)send_all(client_fd,
                   "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n");
    ::close(client_fd);
    return;
  }

  const std::size_t colon = target.rfind(':');
  const std::string host = colon == std::string::npos ? target : target.substr(0, colon);
  const uint16_t port = colon == std::string::npos
                            ? 443
                            : static_cast<uint16_t>(std::stoi(target.substr(colon + 1)));
  if (!host_allowed(options.allowed_hosts, host, port)) {
    ++denied;
    (void)send_all(client_fd,
                   "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    ::close(client_fd);
    return;
  }

  const int upstream = connect_to(host, port);
  if (upstream < 0) {
    ++denied;
    (void)send_all(client_fd,
                   "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
    ::close(client_fd);
    return;
  }
  if (!send_all(client_fd, "HTTP/1.1 200 Connection Established\r\n\r\n")) {
    ::close(upstream);
    ::close(client_fd);
    return;
  }
  ++allowed;

  // Tunnel bytes in both directions until either side closes.
  char tunnel[8192];
  while (true) {
    pollfd fds[2] = {{client_fd, POLLIN, 0}, {upstream, POLLIN, 0}};
    if (::poll(fds, 2, -1) <= 0) {
      break;
    }
    bool closed = false;
    for (int i = 0; i < 2; ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      const int from = fds[i].fd;
      const int to = i == 0 ? upstream : client_fd;
      const ssize_t got = ::recv(from, tunnel, sizeof(tunnel), 0);
      if (got <= 0 || !send_all(to, std::string_view(tunnel, static_cast<std::size_t>(got)))) {
        closed = true;
        break;
      }
    }
    if (closed) {
      break;
    }
  }
  ::close(upstream);
  ::close(client_fd);
}

void EgressProxy::Impl::accept_loop(int listen_fd) {
  // The fd is a local copy: shutdown() closes the member without racing with
  // this loop, and the failed accept ends the loop once running is false.
  while (running.load()) {
    const int client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (running.load()) {
        continue;
      }
      break;
    }
    std::thread([this, client_fd] { serve(client_fd); }).detach();
  }
}

void EgressProxy::Impl::shutdown() {
  if (listen_fd >= 0) {
    ::shutdown(listen_fd, SHUT_RDWR);
    ::close(listen_fd);
    listen_fd = -1;
  }
}

EgressProxy::EgressProxy() : impl_(std::make_unique<Impl>()) {}

EgressProxy::~EgressProxy() { stop(); }

Result<std::unique_ptr<EgressProxy>> EgressProxy::start(ProxyOptions options) {
  auto proxy = std::unique_ptr<EgressProxy>(new EgressProxy());
  proxy->impl_->options = std::move(options);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return fail("PROXY_LISTEN_FAILED", std::string("socket: ") + std::strerror(errno));
  }
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(proxy->impl_->options.port);
  if (::inet_pton(AF_INET, proxy->impl_->options.listen_host.c_str(),
                  &address.sin_addr) != 1) {
    ::close(fd);
    return fail("PROXY_LISTEN_FAILED",
                "invalid listen host: " + proxy->impl_->options.listen_host);
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(fd, 16) != 0) {
    const std::string detail = std::strerror(errno);
    ::close(fd);
    return fail("PROXY_LISTEN_FAILED", "bind/listen: " + detail);
  }
  socklen_t length = sizeof(address);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
  proxy->port_ = ntohs(address.sin_port);
  proxy->impl_->listen_fd = fd;
  proxy->impl_->running = true;
  proxy->impl_->accept_thread =
      std::thread([impl = proxy->impl_.get(), fd] { impl->accept_loop(fd); });
  return proxy;
}

void EgressProxy::stop() {
  if (impl_ == nullptr || !impl_->running.exchange(false)) {
    return;
  }
  impl_->shutdown();
  if (impl_->accept_thread.joinable()) {
    impl_->accept_thread.join();
  }
}

std::size_t EgressProxy::allowed_connections() const { return impl_->allowed.load(); }
std::size_t EgressProxy::denied_requests() const { return impl_->denied.load(); }

}  // namespace cf::sandbox
