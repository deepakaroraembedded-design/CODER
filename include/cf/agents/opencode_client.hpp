// cf-agents: opencode adapter (design section 6.6).
//
// A synchronous HTTP client for the opencode server API plus a background SSE
// event stream. The orchestrator drives one opencode server per agent role;
// this adapter is the only place that knows opencode's wire protocol, so a
// minimal C++ harness can replace opencode without touching the rest of the
// framework (design section 6.6.2, no-go path).
//
// The pinned version (1.18.35) exposes both the documented v1 API
// (/session, /event) and a newer /api/* family. Phase 0a found the /api/*
// session drain is not enabled in this build (POST /api/session/{id}/wait
// returns 503 without a foreground drain), so CF uses the v1 surface, which
// the harness compatibility suite exercises end to end. See
// docs/phase0a/opencode-api-notes.md.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"

namespace cf::agents {

using Json = nlohmann::json;

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kConnectFailed = "OC_CONNECT_FAILED";
inline constexpr const char* kTimeout = "OC_TIMEOUT";
inline constexpr const char* kHttpError = "OC_HTTP_ERROR";
inline constexpr const char* kInvalidJson = "OC_INVALID_JSON";
inline constexpr const char* kStreamFailed = "OC_STREAM_FAILED";
}  // namespace error_codes

struct OpencodeClientOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 4096;
  std::optional<std::string> username;  // HTTP basic auth, optional
  std::optional<std::string> password;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{60000};
};

// One SSE event, unwrapped from its envelope.
struct OpencodeEvent {
  std::string id;
  std::string type;
  Json properties;  // event-specific payload
  Json raw;         // the full envelope as received
};

// SSE parser, exposed separately so it can be unit tested without a socket.
class SseParser {
 public:
  using EventCallback = std::function<void(const OpencodeEvent&)>;

  explicit SseParser(EventCallback callback);

  // Feeds a chunk of raw SSE bytes; dispatches complete events synchronously.
  void feed(std::string_view chunk);
  void reset();

 private:
  void process_line(const std::string& line);
  void dispatch();

  EventCallback callback_;
  std::string buffer_;
  std::string data_;
  std::string event_name_;
  std::string event_id_;
};

// Background SSE stream. The callback runs on the stream's own thread.
class EventStream {
 public:
  using Callback = std::function<void(const OpencodeEvent&)>;
  using DisconnectCallback = std::function<void(const std::string& reason)>;

  virtual ~EventStream() = default;
  virtual void set_callback(Callback callback) = 0;
  virtual void set_disconnect_callback(DisconnectCallback callback) = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;
  // Blocks until the stream is running or has exited. Returns false when the
  // stream never became available within the timeout.
  virtual bool wait_running(std::chrono::milliseconds timeout) = 0;
};

class OpencodeClient {
 public:
  explicit OpencodeClient(OpencodeClientOptions options);
  ~OpencodeClient();
  OpencodeClient(const OpencodeClient&) = delete;
  OpencodeClient& operator=(const OpencodeClient&) = delete;

  // --- Server / session management ---------------------------------------
  Result<Json> health() const;
  // Startup probe: bounds each attempt so callers can retry within their own
  // deadline (opencode accepts TCP before it answers HTTP).
  Result<Json> health(std::chrono::milliseconds timeout) const;
  Result<Json> list_providers() const;
  Result<Json> create_session(const Json& body) const;
  Result<Json> session_info(std::string_view session_id) const;
  Result<Json> session_messages(std::string_view session_id) const;
  Result<Json> list_mcp() const;

  // --- Driving a turn -----------------------------------------------------
  // Admits a prompt asynchronously; completion is signalled by session.idle
  // on the event stream (or observed through wait_idle). model is a v1
  // reference {"providerID": "...", "modelID": "..."} and may be null to use
  // the session's model.
  Result<Json> prompt(std::string_view session_id, std::string_view text,
                      const Json& model = Json(),
                      const std::string& agent = "") const;
  // Polls /session/status until the session is idle or absent. Returns
  // OC_TIMEOUT when the optional timeout expires first.
  Result<void> wait_idle(std::string_view session_id,
                         std::optional<std::chrono::milliseconds> timeout =
                             std::nullopt) const;
  Result<void> interrupt(std::string_view session_id) const;

  // --- Permissions and questions (design section 6.6.1) -------------------
  // reply is "once" or "reject"; "always" is never sent by the framework.
  Result<void> reply_permission(std::string_view session_id,
                                std::string_view request_id,
                                std::string_view reply,
                                std::string_view message = "") const;
  Result<void> reply_question(
      std::string_view session_id, std::string_view request_id,
      const std::vector<std::vector<std::string>>& answers) const;
  Result<void> reject_question(std::string_view session_id,
                               std::string_view request_id) const;

  // --- Raw access (used by the spike and by contract tests) ---------------
  Result<Json> get(std::string_view path) const;
  Result<Json> post(std::string_view path, const Json& body) const;

  // Opens an SSE stream. The caller must keep the returned stream alive and
  // call stop() before destroying the client.
  std::unique_ptr<EventStream> open_event_stream(
      std::string_view path = "/event") const;

  const OpencodeClientOptions& options() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cf::agents
