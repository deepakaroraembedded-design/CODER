// cf-mcp: the shared streamable-HTTP MCP transport (design section 6.4).
//
// A stateless JSON-RPC endpoint at POST /mcp: a single request or a batch is
// answered with application/json; GET (server-initiated messages) is not
// used by this build. The stdio transport remains the per-agent default.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"

namespace cf::mcp {

class HttpServer {
 public:
  using Handler = std::function<nlohmann::json(const nlohmann::json& request)>;

  // Binds `host:port` (port 0 picks a free port) and starts a background
  // accept loop. The handler must stay valid until stop()/destruction.
  static Result<std::unique_ptr<HttpServer>> start(std::string host,
                                                   std::uint16_t port,
                                                   Handler handler);

  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // The bound port (useful when start() was given port 0).
  std::uint16_t port() const;

  // Stops accepting and joins the background thread.
  void stop();

 private:
  struct Impl;
  explicit HttpServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cf::mcp
