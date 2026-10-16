// cf-mcp: MCP server over stdio (design section 6.4).
#include "cf/mcp/stdio_server.hpp"

#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

namespace cf::mcp {

StdioServer::StdioServer(Handler handler) : handler_(std::move(handler)) {}

int StdioServer::run() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    Json request = Json::parse(line, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
      spdlog::warn("cf-mcp: dropping malformed input line ({} bytes)", line.size());
      continue;
    }

    Json response;
    try {
      response = handler_(request);
    } catch (const std::exception& ex) {
      spdlog::error("cf-mcp: handler threw: {}", ex.what());
      if (request.contains("id")) {
        response = Json{{"jsonrpc", "2.0"},
                        {"id", request["id"]},
                        {"error", Json{{"code", -32603}, {"message", ex.what()}}}};
      }
    }

    if (response.is_null()) {
      continue;  // notification: no response
    }

    if (response.contains("__raw__") && response["__raw__"].is_string()) {
      std::cout << response["__raw__"].get<std::string>() << "\n";
    } else {
      std::cout << response.dump() << "\n";
    }
    std::cout.flush();
  }
  return 0;
}

}  // namespace cf::mcp
