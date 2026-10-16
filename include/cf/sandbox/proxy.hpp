// cf-sandbox: the egress allow-listing proxy (design section 6.8.2).
//
// Agent containers need exactly one egress path: the model endpoint. This is
// a minimal HTTP CONNECT proxy bound to the host loopback that tunnels only
// to allow-listed host[:port] patterns and answers 403 to everything else.
// Containers get HTTP_PROXY/HTTPS_PROXY pointing at it through
// host.docker.internal; the kernel-level route restriction (so a process
// cannot bypass the proxy with a direct connection) still comes from the
// sandbox network setup.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::sandbox {

struct ProxyOptions {
  std::string listen_host = "127.0.0.1";
  uint16_t port = 0;  // 0: an ephemeral port
  // Patterns: "host" (any port), "host:port", "*.suffix" or "*.suffix:port".
  std::vector<std::string> allowed_hosts;
};

// True when host:port matches at least one pattern.
bool host_allowed(const std::vector<std::string>& patterns, std::string_view host,
                  uint16_t port);

class EgressProxy {
 public:
  static Result<std::unique_ptr<EgressProxy>> start(ProxyOptions options);
  ~EgressProxy();
  EgressProxy(const EgressProxy&) = delete;
  EgressProxy& operator=(const EgressProxy&) = delete;

  void stop();
  uint16_t port() const { return port_; }
  std::size_t allowed_connections() const;
  std::size_t denied_requests() const;

 private:
  EgressProxy();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;
};

}  // namespace cf::sandbox
