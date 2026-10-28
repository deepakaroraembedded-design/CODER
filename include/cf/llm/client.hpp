// cf-llm: Fireworks / OpenAI-compatible client (design section 6.6).
//
// Direct model calls used for planning, embeddings and reranking (agent turns
// go through opencode instead). One client instance serves one agent role, so
// its concurrency limit is the role's concurrency limit. The client retries
// 429 and 5xx responses (and transport failures) with exponential backoff and
// jitter, honouring Retry-After, and reports every call to the usage callback
// for the cost ledger (design section 6.11).
//
// The HTTP transport is behind an interface so contract tests can inject
// recorded fixtures without a network (design section 17.1).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"

namespace cf::llm {

using Json = nlohmann::json;

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kNoApiKey = "LLM_NO_API_KEY";
inline constexpr const char* kTransportError = "LLM_TRANSPORT_ERROR";
inline constexpr const char* kTimeout = "LLM_TIMEOUT";
inline constexpr const char* kHttpError = "LLM_HTTP_ERROR";
inline constexpr const char* kRetryExhausted = "LLM_RETRY_EXHAUSTED";
inline constexpr const char* kInvalidResponse = "LLM_INVALID_RESPONSE";
inline constexpr const char* kArchiveKey = "LLM_ARCHIVE_KEY";
inline constexpr const char* kArchiveInvalid = "LLM_ARCHIVE_INVALID";
}  // namespace error_codes

struct Usage {
  int64_t prompt_tokens = 0;
  int64_t completion_tokens = 0;
  int64_t cached_tokens = 0;
};

struct ChatMessage {
  std::string role;         // "system" | "user" | "assistant" | "tool"
  std::string content;
  Json tool_calls;          // assistant tool calls, provider shape (null when none)
  std::string tool_call_id; // set for role == "tool"
  std::string name;         // optional tool name for role == "tool"

  Json to_json() const;
};

// Accounting context for one call (design section 6.11: every LLM call is
// recorded with role, model, tokens, latency and cost).
struct CallContext {
  std::string run_id;
  std::string role;
  std::string milestone_id;
  std::string prompt_ref;  // artifact reference for the archived prompt
};

struct ChatRequest {
  std::string model;  // e.g. accounts/fireworks/models/deepseek-v4p1-flash
  std::vector<ChatMessage> messages;
  Json tools;  // null, or an array of tool definitions
  std::optional<double> temperature;
  std::optional<int64_t> max_tokens;
  std::optional<int64_t> seed;
  CallContext context;
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments_raw;  // JSON text exactly as the model emitted it
  Json arguments;             // parsed when the text is valid JSON
};

struct ChatResponse {
  std::string id;
  std::string model;
  std::string content;  // assistant text; empty when the turn was tool calls
  std::vector<ToolCall> tool_calls;
  Usage usage;
  Json raw;  // the full provider response
};

struct EmbedRequest {
  std::string model;
  std::vector<std::string> input;
  CallContext context;
};

struct EmbedResponse {
  std::vector<std::vector<float>> embeddings;
  Usage usage;
  Json raw;
};

struct HttpRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::chrono::milliseconds timeout{120000};
};

struct HttpResponse {
  long status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  std::optional<std::string> header(std::string_view name) const;
};

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual Result<HttpResponse> post(const HttpRequest& request) = 0;
  // Downloads (the reference mirror); transports that only implement POST
  // return an error.
  virtual Result<HttpResponse> get(const HttpRequest& request);
  // GitHub updates use PATCH; the default reports it as unsupported.
  virtual Result<HttpResponse> patch(const HttpRequest& request);
  // Deleting duplicate side effects after a late landing (design section
  // 6.12.6 rule 4) uses DELETE; the default reports it as unsupported.
  virtual Result<HttpResponse> del(const HttpRequest& request);
};

// libcurl-backed transport (design section 6.6: cpr).
class CprTransport final : public HttpTransport {
 public:
  Result<HttpResponse> post(const HttpRequest& request) override;
  Result<HttpResponse> get(const HttpRequest& request) override;
  Result<HttpResponse> patch(const HttpRequest& request) override;
  Result<HttpResponse> del(const HttpRequest& request) override;
};

// Price per million tokens, used to turn usage into dollars for the ledger.
// Models without an entry are recorded with usd = 0.
struct ModelPrice {
  double usd_per_mtok_in = 0.0;
  double usd_per_mtok_out = 0.0;
};

struct ClientOptions {
  std::string base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key;
  std::chrono::milliseconds request_timeout{120000};
  int max_attempts = 6;  // 1 initial attempt + up to 5 retries
  std::chrono::milliseconds initial_backoff{500};
  std::chrono::milliseconds max_backoff{30000};
  int max_concurrency = 4;  // per role (design section 6.6)
  std::map<std::string, ModelPrice, std::less<>> prices;
  // Encrypted prompt/response archive (design sections 6.11, 14). Empty
  // disables archiving; the key is read from archive_key_env as 64 hex
  // characters (32 bytes). The archive is gzip-compressed and AES-256-GCM
  // encrypted.
  std::filesystem::path archive_dir;
  std::string archive_key_env = "CF_ARCHIVE_KEY";
};

struct CallRecord {
  std::string run_id;
  std::string role;
  std::string model;
  std::string prompt_ref;
  std::string response_ref;
  Usage usage;
  double usd = 0.0;
  int64_t latency_ms = 0;
  int attempts = 1;
  long status = 0;  // 0 when the request never reached the provider
};

using UsageCallback = std::function<void(const CallRecord&)>;
using SleepFunction = std::function<void(std::chrono::milliseconds)>;

// One archived chat/embedding call, decrypted and decompressed.
struct ArchivedCall {
  std::string call_id;
  std::string run_id;
  std::string role;
  std::string milestone_id;
  std::string model;
  std::string base_url;
  std::string path;
  nlohmann::json request;
  nlohmann::json response;
  long status = 0;
  Usage usage;
  int64_t at = 0;
};

// Reads and verifies an archive file. The key comes from CF_ARCHIVE_KEY.
Result<ArchivedCall> read_archived_call(const std::filesystem::path& path);

class FireworksClient {
 public:
  // `on_call` receives one record per logical call (success or final
  // failure). `sleep` is injectable so tests do not actually wait.
  FireworksClient(ClientOptions options, std::unique_ptr<HttpTransport> transport,
                  UsageCallback on_call = {}, SleepFunction sleep = {});
  ~FireworksClient();
  FireworksClient(const FireworksClient&) = delete;
  FireworksClient& operator=(const FireworksClient&) = delete;

  Result<ChatResponse> chat(const ChatRequest& request);
  Result<EmbedResponse> embed(const EmbedRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cf::llm
