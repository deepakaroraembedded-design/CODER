#include "cf/llm/client.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

#include <cpr/cpr.h>

namespace cf::llm {
namespace {

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string body_snippet(const std::string& body, std::size_t max = 300) {
  if (body.size() <= max) {
    return body;
  }
  return body.substr(0, max) + "...";
}

Usage parse_usage(const Json& usage_json) {
  Usage usage;
  if (!usage_json.is_object()) {
    return usage;
  }
  usage.prompt_tokens = usage_json.value("prompt_tokens", int64_t{0});
  usage.completion_tokens = usage_json.value("completion_tokens", int64_t{0});
  const auto details = usage_json.find("prompt_tokens_details");
  if (details != usage_json.end() && details->is_object()) {
    usage.cached_tokens = details->value("cached_tokens", int64_t{0});
  }
  return usage;
}

std::optional<std::chrono::milliseconds> parse_retry_after(
    const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  try {
    const long seconds = std::stol(*value);
    if (seconds < 0) {
      return std::nullopt;
    }
    // Cap the wait so a hostile or mistaken header cannot stall a run.
    return std::chrono::milliseconds(std::min<long>(seconds, 300) * 1000);
  } catch (const std::exception&) {
    return std::nullopt;  // HTTP-date form is not supported; use backoff
  }
}

}  // namespace

Json ChatMessage::to_json() const {
  Json message = {{"role", role}, {"content", content}};
  if (!tool_calls.is_null()) {
    message["tool_calls"] = tool_calls;
  }
  if (!tool_call_id.empty()) {
    message["tool_call_id"] = tool_call_id;
  }
  if (!name.empty()) {
    message["name"] = name;
  }
  return message;
}

std::optional<std::string> HttpResponse::header(std::string_view name) const {
  const std::string wanted = to_lower(name);
  for (const auto& [key, value] : headers) {
    if (to_lower(key) == wanted) {
      return value;
    }
  }
  return std::nullopt;
}

Result<HttpResponse> HttpTransport::get(const HttpRequest& request) {
  (void)request;
  return fail(error_codes::kTransportError,
              "this transport does not implement downloads");
}

Result<HttpResponse> HttpTransport::patch(const HttpRequest& request) {
  (void)request;
  return fail(error_codes::kTransportError,
              "this transport does not implement PATCH");
}

Result<HttpResponse> CprTransport::post(const HttpRequest& request) {
  cpr::Header headers;
  for (const auto& [key, value] : request.headers) {
    headers[key] = value;
  }
  const cpr::Response response = cpr::Post(cpr::Url{request.url}, headers,
                                           cpr::Body{request.body},
                                           cpr::Timeout{request.timeout});
  if (response.error.code != cpr::ErrorCode::OK) {
    if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
      return fail(error_codes::kTimeout, response.error.message);
    }
    return fail(error_codes::kTransportError, response.error.message);
  }
  HttpResponse out;
  out.status = response.status_code;
  out.headers.reserve(response.header.size());
  for (const auto& [key, value] : response.header) {
    out.headers.emplace_back(key, value);
  }
  out.body = response.text;
  return out;
}

Result<HttpResponse> CprTransport::get(const HttpRequest& request) {
  cpr::Header headers;
  for (const auto& [key, value] : request.headers) {
    headers[key] = value;
  }
  const cpr::Response response = cpr::Get(cpr::Url{request.url}, headers,
                                          cpr::Timeout{request.timeout});
  if (response.error.code != cpr::ErrorCode::OK) {
    if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
      return fail(error_codes::kTimeout, response.error.message);
    }
    return fail(error_codes::kTransportError, response.error.message);
  }
  HttpResponse out;
  out.status = response.status_code;
  out.headers.reserve(response.header.size());
  for (const auto& [key, value] : response.header) {
    out.headers.emplace_back(key, value);
  }
  out.body = response.text;
  return out;
}

Result<HttpResponse> CprTransport::patch(const HttpRequest& request) {
  cpr::Header headers;
  for (const auto& [key, value] : request.headers) {
    headers[key] = value;
  }
  const cpr::Response response = cpr::Patch(cpr::Url{request.url}, headers,
                                            cpr::Body{request.body},
                                            cpr::Timeout{request.timeout});
  if (response.error.code != cpr::ErrorCode::OK) {
    if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
      return fail(error_codes::kTimeout, response.error.message);
    }
    return fail(error_codes::kTransportError, response.error.message);
  }
  HttpResponse out;
  out.status = response.status_code;
  out.headers.reserve(response.header.size());
  for (const auto& [key, value] : response.header) {
    out.headers.emplace_back(key, value);
  }
  out.body = response.text;
  return out;
}

struct FireworksClient::Impl {
  Impl(ClientOptions opts, std::unique_ptr<HttpTransport> transport_in,
       UsageCallback callback, SleepFunction sleeper)
      : options(std::move(opts)),
        transport(std::move(transport_in)),
        on_call(std::move(callback)),
        sleep(std::move(sleeper)),
        rng(std::random_device{}()) {
    if (options.max_attempts < 1) {
      options.max_attempts = 1;
    }
    if (options.max_concurrency < 1) {
      options.max_concurrency = 1;
    }
    if (options.initial_backoff.count() < 1) {
      options.initial_backoff = std::chrono::milliseconds(1);
    }
    if (options.max_backoff < options.initial_backoff) {
      options.max_backoff = options.initial_backoff;
    }
    if (!sleep) {
      sleep = [](std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); };
    }
  }

  // Per-role concurrency limit (design section 6.6).
  struct Slot {
    explicit Slot(Impl& owner) : impl(owner) { impl.acquire_slot(); }
    ~Slot() { impl.release_slot(); }
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Impl& impl;
  };

  void acquire_slot() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return in_flight < options.max_concurrency; });
    ++in_flight;
  }

  void release_slot() {
    std::lock_guard<std::mutex> lock(mutex);
    --in_flight;
    cv.notify_one();
  }

  // Exponential backoff with full jitter (design section 6.6).
  std::chrono::milliseconds backoff_for(int attempt) {
    const int64_t base = std::max<int64_t>(1, options.initial_backoff.count());
    const int64_t cap = options.max_backoff.count();
    const int shift = std::min(attempt - 1, 30);
    const int64_t window = std::min(cap, base << shift);
    std::uniform_int_distribution<int64_t> dist(0, window);
    return std::chrono::milliseconds(dist(rng));
  }

  double compute_usd(const std::string& model, const Usage& usage) const {
    const auto price = options.prices.find(model);
    if (price == options.prices.end()) {
      return 0.0;
    }
    return (static_cast<double>(usage.prompt_tokens) / 1e6) *
               price->second.usd_per_mtok_in +
           (static_cast<double>(usage.completion_tokens) / 1e6) *
               price->second.usd_per_mtok_out;
  }

  void finish(CallRecord& record, int attempts, long status, const Usage& usage,
              std::chrono::steady_clock::time_point start) {
    record.attempts = attempts;
    record.status = status;
    record.usage = usage;
    record.usd = compute_usd(record.model, usage);
    record.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    if (on_call) {
      on_call(record);
    }
  }

  Result<Json> post_json(std::string_view path, const Json& body,
                         const CallContext& context, const std::string& model,
                         CallRecord& record) {
    record.run_id = context.run_id;
    record.role = context.role;
    record.model = model;
    record.prompt_ref = context.prompt_ref;

    if (options.api_key.empty()) {
      return fail(error_codes::kNoApiKey, "no API key configured");
    }

    Slot slot(*this);

    HttpRequest request;
    request.url = options.base_url + std::string(path);
    request.headers = {{"Authorization", "Bearer " + options.api_key},
                       {"Content-Type", "application/json"},
                       {"Accept", "application/json"}};
    request.body = body.dump();
    request.timeout = options.request_timeout;

    const auto start = std::chrono::steady_clock::now();
    int attempts = 0;
    std::string last_error;
    long last_status = 0;

    while (attempts < options.max_attempts) {
      ++attempts;
      auto response = transport->post(request);
      if (!response) {
        last_status = 0;
        last_error = response.error().code + ": " + response.error().message;
        if (attempts >= options.max_attempts) {
          finish(record, attempts, last_status, Usage{}, start);
          return fail(error_codes::kRetryExhausted,
                      "transport failed after " + std::to_string(attempts) +
                          " attempts: " + last_error);
        }
        sleep(backoff_for(attempts));
        continue;
      }

      last_status = response->status;
      if (last_status == 200 || last_status == 201) {
        Json parsed = Json::parse(response->body, nullptr, false);
        if (parsed.is_discarded()) {
          finish(record, attempts, last_status, Usage{}, start);
          return fail(error_codes::kInvalidResponse,
                      "response body is not valid JSON: " + body_snippet(response->body));
        }
        Usage usage;
        const auto usage_it = parsed.find("usage");
        if (usage_it != parsed.end()) {
          usage = parse_usage(*usage_it);
        }
        finish(record, attempts, last_status, usage, start);
        return parsed;
      }

      last_error = "HTTP " + std::to_string(last_status) + ": " +
                   body_snippet(response->body);
      const bool retryable = last_status == 408 || last_status == 429 ||
                             (last_status >= 500 && last_status <= 599);
      if (!retryable || attempts >= options.max_attempts) {
        finish(record, attempts, last_status, Usage{}, start);
        return fail(retryable ? error_codes::kRetryExhausted : error_codes::kHttpError,
                    last_error);
      }
      const auto retry_after = parse_retry_after(response->header("Retry-After"));
      sleep(retry_after.value_or(backoff_for(attempts)));
    }

    // Unreachable: the loop returns at max_attempts.
    finish(record, attempts, last_status, Usage{}, start);
    return fail(error_codes::kRetryExhausted, last_error);
  }

  ClientOptions options;
  std::unique_ptr<HttpTransport> transport;
  UsageCallback on_call;
  SleepFunction sleep;
  std::mt19937_64 rng;
  std::mutex mutex;
  std::condition_variable cv;
  int in_flight = 0;
};

FireworksClient::FireworksClient(ClientOptions options,
                                 std::unique_ptr<HttpTransport> transport,
                                 UsageCallback on_call, SleepFunction sleep)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(transport),
                                   std::move(on_call), std::move(sleep))) {}

FireworksClient::~FireworksClient() = default;

Result<ChatResponse> FireworksClient::chat(const ChatRequest& request) {
  Json body;
  body["model"] = request.model;
  Json messages = Json::array();
  for (const auto& message : request.messages) {
    messages.push_back(message.to_json());
  }
  body["messages"] = std::move(messages);
  if (!request.tools.is_null()) {
    body["tools"] = request.tools;
  }
  if (request.temperature.has_value()) {
    body["temperature"] = *request.temperature;
  }
  if (request.max_tokens.has_value()) {
    body["max_tokens"] = *request.max_tokens;
  }
  if (request.seed.has_value()) {
    body["seed"] = *request.seed;
  }

  CallRecord record;
  auto parsed = impl_->post_json("/chat/completions", body, request.context,
                                 request.model, record);
  if (!parsed) {
    return tl::make_unexpected(parsed.error());
  }

  ChatResponse response;
  response.raw = *parsed;
  response.id = parsed->value("id", "");
  response.model = parsed->value("model", request.model);
  response.usage = record.usage;

  const auto choices = parsed->find("choices");
  if (choices == parsed->end() || !choices->is_array() || choices->empty()) {
    return fail(error_codes::kInvalidResponse, "chat response has no choices");
  }
  const auto message = choices->front().find("message");
  if (message == choices->front().end() || !message->is_object()) {
    return fail(error_codes::kInvalidResponse, "chat choice has no message");
  }
  const auto content = message->find("content");
  if (content != message->end() && content->is_string()) {
    response.content = content->get<std::string>();
  }
  const auto tool_calls = message->find("tool_calls");
  if (tool_calls != message->end() && tool_calls->is_array()) {
    for (const auto& call : *tool_calls) {
      ToolCall tool_call;
      tool_call.id = call.value("id", "");
      const auto function = call.find("function");
      if (function != call.end() && function->is_object()) {
        tool_call.name = function->value("name", "");
        tool_call.arguments_raw = function->value("arguments", "");
        tool_call.arguments = Json::parse(tool_call.arguments_raw, nullptr, false);
        if (tool_call.arguments.is_discarded()) {
          tool_call.arguments = Json();
        }
      }
      response.tool_calls.push_back(std::move(tool_call));
    }
  }
  return response;
}

Result<EmbedResponse> FireworksClient::embed(const EmbedRequest& request) {
  Json body = {{"model", request.model}, {"input", request.input}};

  CallRecord record;
  auto parsed = impl_->post_json("/embeddings", body, request.context, request.model,
                                 record);
  if (!parsed) {
    return tl::make_unexpected(parsed.error());
  }

  EmbedResponse response;
  response.raw = *parsed;
  response.usage = record.usage;

  const auto data = parsed->find("data");
  if (data == parsed->end() || !data->is_array()) {
    return fail(error_codes::kInvalidResponse, "embedding response has no data array");
  }
  for (const auto& item : *data) {
    const auto embedding = item.find("embedding");
    if (embedding == item.end() || !embedding->is_array()) {
      return fail(error_codes::kInvalidResponse, "embedding item has no embedding array");
    }
    std::vector<float> vector;
    vector.reserve(embedding->size());
    for (const auto& value : *embedding) {
      if (!value.is_number()) {
        return fail(error_codes::kInvalidResponse,
                    "embedding vector contains a non-number");
      }
      vector.push_back(value.get<float>());
    }
    response.embeddings.push_back(std::move(vector));
  }
  return response;
}

}  // namespace cf::llm
