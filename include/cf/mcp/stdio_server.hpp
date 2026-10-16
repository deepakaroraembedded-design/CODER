// cf-mcp: MCP server over stdio (design section 6.4).
//
// Newline-delimited JSON-RPC 2.0 on stdin/stdout; all logging goes to stderr.
// The transport loop is generic; the tool set is provided by the caller.
#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace cf::mcp {

using Json = nlohmann::json;

class StdioServer {
 public:
  // Returns the JSON-RPC response object for a request, or a null Json for
  // notifications (no response is written). A response containing the string
  // field "__raw__" is written verbatim, which the spike uses to inject
  // malformed output (scenario H5).
  using Handler = std::function<Json(const Json& request)>;

  explicit StdioServer(Handler handler);

  // Reads until EOF. Returns a process exit code.
  int run();

 private:
  Handler handler_;
};

}  // namespace cf::mcp
