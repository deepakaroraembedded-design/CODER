// cf-agents: opencode adapter implementation (design section 6.6).
#include "cf/agents/opencode_client.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include <spdlog/spdlog.h>

namespace cf::agents {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

std::string base64_encode(std::string_view in) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const auto v = (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 2]));
    out += kTable[(v >> 18) & 0x3F];
    out += kTable[(v >> 12) & 0x3F];
    out += kTable[(v >> 6) & 0x3F];
    out += kTable[v & 0x3F];
    i += 3;
  }
  const auto remaining = in.size() - i;
  if (remaining == 1) {
    const auto v = static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16;
    out += kTable[(v >> 18) & 0x3F];
    out += kTable[(v >> 12) & 0x3F];
    out += "==";
  } else if (remaining == 2) {
    const auto v = (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8);
    out += kTable[(v >> 18) & 0x3F];
    out += kTable[(v >> 12) & 0x3F];
    out += kTable[(v >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

struct HttpResponse {
  unsigned status = 0;
  std::string body;
};

// Synchronous-style request built from asynchronous Beast operations: the
// stream's expiry timer only fires while the io_context runs, so timeouts
// actually bound connect/write/read (a synchronous read would ignore them and
// block forever against a server that accepts but does not answer yet, which
// is exactly what opencode does while it starts up).
Result<HttpResponse> do_request(const OpencodeClientOptions& options,
                                http::verb verb, const std::string& target,
                                const std::string& body,
                                std::chrono::milliseconds timeout) {
  asio::io_context ioc;
  tcp::resolver resolver(ioc);
  beast::error_code ec;
  const auto endpoints = resolver.resolve(options.host, std::to_string(options.port), ec);
  if (ec) {
    return fail(error_codes::kConnectFailed,
                "resolve " + options.host + ":" + std::to_string(options.port) +
                    ": " + ec.message());
  }

  beast::tcp_stream stream(ioc);
  stream.expires_after(options.connect_timeout);
  stream.async_connect(endpoints,
                       [&ec](beast::error_code connect_ec, const tcp::endpoint&) {
                         ec = connect_ec;
                       });
  ioc.run();
  if (ec) {
    return fail(error_codes::kConnectFailed, "connect: " + ec.message());
  }

  http::request<http::string_body> req{verb, target, 11};
  req.set(http::field::host, options.host + ":" + std::to_string(options.port));
  req.set(http::field::user_agent, "cf-opencode-adapter/0.1");
  req.set(http::field::accept, "application/json");
  if (options.username && options.password) {
    req.set(http::field::authorization,
            "Basic " + base64_encode(options.username.value() + ":" + options.password.value()));
  }
  if (!body.empty()) {
    req.body() = body;
    req.set(http::field::content_type, "application/json");
  }
  req.prepare_payload();

  stream.expires_after(timeout);
  http::async_write(stream, req,
                    [&ec](beast::error_code write_ec, std::size_t) { ec = write_ec; });
  ioc.restart();
  ioc.run();
  if (ec) {
    if (ec == beast::error::timeout) {
      return fail(error_codes::kTimeout,
                  "request timed out: " + std::string(http::to_string(verb)) + " " +
                      target);
    }
    return fail(error_codes::kConnectFailed, "write: " + ec.message());
  }

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  stream.expires_after(timeout);
  http::async_read(stream, buffer, res,
                   [&ec](beast::error_code read_ec, std::size_t) { ec = read_ec; });
  ioc.restart();
  ioc.run();
  if (ec == beast::error::timeout) {
    return fail(error_codes::kTimeout,
                "request timed out: " + std::string(http::to_string(verb)) + " " + target);
  }
  if (ec) {
    return fail(error_codes::kConnectFailed, "read: " + ec.message());
  }

  HttpResponse out;
  out.status = res.result_int();
  out.body = res.body();
  return out;
}

Result<Json> parse_json(const HttpResponse& response) {
  if (response.body.empty()) {
    return Json();
  }
  Json parsed = Json::parse(response.body, nullptr, false);
  if (parsed.is_discarded()) {
    return fail(error_codes::kInvalidJson,
                "invalid JSON in response: " + response.body.substr(0, 200));
  }
  return parsed;
}

OpencodeEvent make_event(std::string id, std::string name, const std::string& data) {
  OpencodeEvent event;
  event.id = std::move(id);
  event.type = std::move(name);

  Json parsed = Json::parse(data, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    event.type = "spike.parse_error";
    event.properties = Json{{"raw", data}};
    event.raw = event.properties;
    return event;
  }

  // GlobalEvent wraps the native event under "payload".
  if (parsed.contains("payload") && parsed["payload"].is_object() &&
      parsed["payload"].contains("type")) {
    parsed = parsed["payload"];
  }

  event.raw = parsed;
  if (parsed.contains("type") && parsed["type"].is_string()) {
    event.type = parsed["type"].get<std::string>();
  }
  if (event.id.empty() && parsed.contains("id") && parsed["id"].is_string()) {
    event.id = parsed["id"].get<std::string>();
  }
  if (parsed.contains("properties") && parsed["properties"].is_object()) {
    event.properties = parsed["properties"];
  } else {
    event.properties = parsed;
  }
  return event;
}

// Background SSE reader. One connection, blocking reads with a short expiry so
// stop() is observed promptly; a read error is reported through the
// disconnect callback (design section 6.6.2, H7/H8).
class SseEventStream final : public EventStream {
 public:
  SseEventStream(OpencodeClientOptions options, std::string path)
      : options_(std::move(options)), path_(std::move(path)),
        parser_([this](const OpencodeEvent& event) {
          EventStream::Callback callback;
          {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = callback_;
          }
          if (callback) {
            callback(event);
          }
        }) {}

  ~SseEventStream() override { stop(); }

  void set_callback(Callback callback) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(callback);
  }

  void set_disconnect_callback(DisconnectCallback callback) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    disconnect_callback_ = std::move(callback);
  }

  void start() { thread_ = std::thread([this] { run(); }); }

  void stop() override {
    stop_.store(true);
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
      thread_.join();
    }
  }

  bool running() const override { return running_.load(); }

  bool wait_running(std::chrono::milliseconds timeout) override {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (running_.load()) {
        return true;
      }
      if (finished_.load()) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return running_.load();
  }

 private:
  void notify_disconnect(const std::string& reason) {
    DisconnectCallback callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = disconnect_callback_;
    }
    if (callback) {
      callback(reason);
    }
  }

  void run() {
    run_inner();
    finished_.store(true);
  }

  void run_inner() {
    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::error_code ec;
    const auto endpoints = resolver.resolve(options_.host, std::to_string(options_.port), ec);
    if (ec) {
      notify_disconnect("resolve: " + ec.message());
      return;
    }

    // Connect and read the response header with real timeouts (the stream's
    // expiry timer only fires while the io_context runs).
    beast::tcp_stream stream(ioc);
    stream.expires_after(options_.connect_timeout);
    stream.async_connect(endpoints,
                         [&ec](beast::error_code connect_ec, const tcp::endpoint&) {
                           ec = connect_ec;
                         });
    ioc.run();
    if (ec) {
      notify_disconnect("connect: " + ec.message());
      return;
    }

    http::request<http::empty_body> req{http::verb::get, path_, 11};
    req.set(http::field::host, options_.host + ":" + std::to_string(options_.port));
    req.set(http::field::user_agent, "cf-opencode-adapter/0.1");
    req.set(http::field::accept, "text/event-stream");
    if (options_.username && options_.password) {
      req.set(http::field::authorization, "Basic " +
              base64_encode(options_.username.value() + ":" + options_.password.value()));
    }
    stream.expires_after(options_.connect_timeout);
    http::async_write(stream, req,
                      [&ec](beast::error_code write_ec, std::size_t) { ec = write_ec; });
    ioc.restart();
    ioc.run();
    if (ec) {
      notify_disconnect("write: " + ec.message());
      return;
    }

    beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
    stream.expires_after(options_.connect_timeout);
    http::async_read_header(
        stream, buffer, parser,
        [&ec](beast::error_code header_ec, std::size_t) { ec = header_ec; });
    ioc.restart();
    ioc.run();
    if (ec) {
      notify_disconnect("read_header: " + ec.message());
      return;
    }
    if (parser.get().result() != http::status::ok) {
      notify_disconnect("http status " + std::to_string(parser.get().result_int()));
      return;
    }

    // Body: poll the non-blocking socket. A beast read timeout would close
    // the socket, so the stop flag is checked between polls instead.
    stream.expires_never();
    stream.socket().non_blocking(true, ec);
    if (ec) {
      notify_disconnect("non_blocking: " + ec.message());
      return;
    }

    running_.store(true);
    std::string disconnect_reason;
    char chunk[8192];
    while (!stop_.load()) {
      parser.get().body().data = chunk;
      parser.get().body().size = sizeof(chunk);
      ec = {};
      http::read_some(stream, buffer, parser, ec);
      if (ec == asio::error::would_block || ec == asio::error::try_again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;  // no data yet; re-check the stop flag
      }
      const auto consumed = sizeof(chunk) - parser.get().body().size;
      if (consumed > 0) {
        parser_.feed(std::string_view(chunk, consumed));
      }
      if (ec == http::error::need_buffer) {
        continue;
      }
      if (ec) {
        disconnect_reason = ec.message();
        break;
      }
      if (parser.is_done()) {
        disconnect_reason = "stream ended";
        break;
      }
    }
    running_.store(false);
    if (!stop_.load()) {
      notify_disconnect(disconnect_reason.empty() ? "stream closed" : disconnect_reason);
    }
  }

  OpencodeClientOptions options_;
  std::string path_;
  SseParser parser_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::thread thread_;
  std::mutex callback_mutex_;
  Callback callback_;
  DisconnectCallback disconnect_callback_;
};

}  // namespace

// --- SseParser --------------------------------------------------------------

SseParser::SseParser(EventCallback callback) : callback_(std::move(callback)) {}

void SseParser::reset() {
  buffer_.clear();
  data_.clear();
  event_name_.clear();
  event_id_.clear();
}

void SseParser::feed(std::string_view chunk) {
  buffer_.append(chunk.data(), chunk.size());
  std::size_t pos = 0;
  while ((pos = buffer_.find('\n')) != std::string::npos) {
    std::string line = buffer_.substr(0, pos);
    buffer_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    process_line(line);
  }
}

void SseParser::process_line(const std::string& line) {
  if (line.empty()) {
    dispatch();
    return;
  }
  if (line[0] == ':') {
    return;  // comment
  }
  const auto colon = line.find(':');
  const std::string field = colon == std::string::npos ? line : line.substr(0, colon);
  std::string value;
  if (colon != std::string::npos) {
    value = line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') {
      value.erase(0, 1);
    }
  }
  if (field == "data") {
    if (!data_.empty()) {
      data_ += '\n';
    }
    data_ += value;
  } else if (field == "event") {
    event_name_ = value;
  } else if (field == "id") {
    event_id_ = value;
  }
}

void SseParser::dispatch() {
  if (data_.empty() && event_name_.empty()) {
    return;
  }
  if (!data_.empty() && callback_) {
    callback_(make_event(event_id_, event_name_, data_));
  }
  data_.clear();
  event_name_.clear();
}

// --- OpencodeClient ---------------------------------------------------------

struct OpencodeClient::Impl {
  explicit Impl(OpencodeClientOptions opts) : options(std::move(opts)) {}
  OpencodeClientOptions options;
};

OpencodeClient::OpencodeClient(OpencodeClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

OpencodeClient::~OpencodeClient() = default;

const OpencodeClientOptions& OpencodeClient::options() const { return impl_->options; }

Result<Json> OpencodeClient::get(std::string_view path) const {
  auto response = do_request(impl_->options, http::verb::get, std::string(path), "",
                             impl_->options.request_timeout);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (response->status < 200 || response->status >= 300) {
    return fail(error_codes::kHttpError,
                "HTTP " + std::to_string(response->status) + " for GET " +
                    std::string(path) + ": " + response->body.substr(0, 300));
  }
  return parse_json(*response);
}

Result<Json> OpencodeClient::post(std::string_view path, const Json& body) const {
  const std::string payload = body.is_null() ? std::string("{}") : body.dump();
  auto response = do_request(impl_->options, http::verb::post, std::string(path), payload,
                             impl_->options.request_timeout);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (response->status < 200 || response->status >= 300) {
    return fail(error_codes::kHttpError,
                "HTTP " + std::to_string(response->status) + " for POST " +
                    std::string(path) + ": " + response->body.substr(0, 300));
  }
  return parse_json(*response);
}

Result<Json> OpencodeClient::health() const { return get("/global/health"); }

Result<Json> OpencodeClient::health(std::chrono::milliseconds timeout) const {
  auto response = do_request(impl_->options, http::verb::get, "/global/health", "",
                             timeout);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (response->status < 200 || response->status >= 300) {
    return fail(error_codes::kHttpError,
                "HTTP " + std::to_string(response->status) +
                    " for GET /global/health: " + response->body.substr(0, 300));
  }
  return parse_json(*response);
}

Result<Json> OpencodeClient::list_providers() const { return get("/config/providers"); }

Result<Json> OpencodeClient::create_session(const Json& body) const {
  return post("/session", body);
}

Result<Json> OpencodeClient::session_info(std::string_view session_id) const {
  return get("/session/" + std::string(session_id));
}

Result<Json> OpencodeClient::session_messages(std::string_view session_id) const {
  return get("/session/" + std::string(session_id) + "/message");
}

Result<Json> OpencodeClient::list_mcp() const { return get("/mcp"); }

Result<Json> OpencodeClient::prompt(std::string_view session_id, std::string_view text,
                                    const Json& model, const std::string& agent) const {
  Json body;
  body["parts"] = Json::array({Json{{"type", "text"}, {"text", std::string(text)}}});
  if (!model.is_null()) {
    body["model"] = model;
  }
  if (!agent.empty()) {
    body["agent"] = agent;
  }
  return post("/session/" + std::string(session_id) + "/prompt_async", body);
}

Result<void> OpencodeClient::wait_idle(
    std::string_view session_id,
    std::optional<std::chrono::milliseconds> timeout) const {
  const auto start = std::chrono::steady_clock::now();
  const auto deadline =
      start + timeout.value_or(std::chrono::milliseconds{3600000});
  // A session is absent from /session/status while it is idle, but it is also
  // briefly absent right after the prompt is admitted (before the turn is
  // registered as busy). Only trust an absent session once it was observed
  // busy, was listed as explicitly idle, or the settle window has passed.
  constexpr auto kSettle = std::chrono::milliseconds(2000);
  bool seen_busy = false;
  bool seen_idle = false;
  for (;;) {
    auto status = get("/session/status");
    if (!status) {
      return tl::make_unexpected(status.error());
    }
    bool idle = true;
    if (status->is_object()) {
      const auto it = status->find(std::string(session_id));
      if (it != status->end() && it->is_object()) {
        const std::string type = it->value("type", "idle");
        idle = (type == "idle");
        if (idle) {
          seen_idle = true;
        } else {
          seen_busy = true;
        }
      }
    }
    if (idle && (seen_busy || seen_idle ||
                 std::chrono::steady_clock::now() - start >= kSettle)) {
      return {};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return fail(error_codes::kTimeout,
                  "session " + std::string(session_id) + " did not become idle");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

Result<void> OpencodeClient::interrupt(std::string_view session_id) const {
  auto response = post("/session/" + std::string(session_id) + "/abort", Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Result<void> OpencodeClient::reply_permission(std::string_view session_id,
                                              std::string_view request_id,
                                              std::string_view reply,
                                              std::string_view message) const {
  (void)message;  // the v1 permission reply carries only the response
  Json body;
  body["response"] = std::string(reply);
  auto response = post("/session/" + std::string(session_id) + "/permissions/" +
                           std::string(request_id),
                       body);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Result<void> OpencodeClient::reply_question(
    std::string_view session_id, std::string_view request_id,
    const std::vector<std::vector<std::string>>& answers) const {
  (void)session_id;  // the v1 question reply is keyed by request ID only
  Json body;
  body["answers"] = answers;
  auto response = post("/question/" + std::string(request_id) + "/reply", body);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Result<void> OpencodeClient::reject_question(std::string_view session_id,
                                             std::string_view request_id) const {
  (void)session_id;
  auto response = post("/question/" + std::string(request_id) + "/reject", Json());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

std::unique_ptr<EventStream> OpencodeClient::open_event_stream(std::string_view path) const {
  auto stream = std::make_unique<SseEventStream>(impl_->options, std::string(path));
  stream->start();
  return stream;
}

}  // namespace cf::agents
