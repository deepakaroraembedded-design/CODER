#include "cf/mcp/http_server.hpp"

#include <atomic>
#include <thread>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

namespace cf::mcp {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr std::size_t kMaxBodyBytes = 8 * 1024 * 1024;

void write_response(tcp::socket& socket, const http::response<http::string_body>& response,
                    beast::error_code& ec) {
  http::write(socket, response, ec);
}

}  // namespace

struct HttpServer::Impl {
  net::io_context context;
  tcp::acceptor acceptor{context};
  Handler handler;
  std::thread thread;
  std::atomic<bool> stopping{false};

  explicit Impl(Handler handler_in) : handler(std::move(handler_in)) {}

  void accept_next() {
    if (stopping.load()) {
      return;
    }
    acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
      if (!ec && !stopping.load()) {
        serve(std::move(socket));
      }
      accept_next();
    });
  }

  void serve(tcp::socket socket) {
    beast::error_code ec;
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request, ec);
    if (ec) {
      return;
    }
    auto respond = [&](http::status status, std::string body,
                       const char* content_type) {
      http::response<http::string_body> response{status, request.version()};
      response.set(http::field::server, "cf-mcp");
      response.set(http::field::content_type, content_type);
      response.keep_alive(false);
      response.body() = std::move(body);
      response.prepare_payload();
      write_response(socket, response, ec);
    };

    if (request.method() != http::verb::post || request.target() != "/mcp") {
      respond(http::status::method_not_allowed, "{}", "application/json");
      return;
    }
    if (request.body().size() > kMaxBodyBytes) {
      respond(http::status::payload_too_large, "{}", "application/json");
      return;
    }
    const nlohmann::json parsed =
        nlohmann::json::parse(request.body(), nullptr, false);
    if (parsed.is_discarded()) {
      respond(http::status::bad_request,
              nlohmann::json{{"jsonrpc", "2.0"},
                             {"id", nullptr},
                             {"error", nlohmann::json{{"code", -32700},
                                                      {"message", "parse error"}}}}
                  .dump(),
              "application/json");
      return;
    }

    auto dispatch = [this](const nlohmann::json& message) -> nlohmann::json {
      if (!message.is_object()) {
        return nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", nullptr},
                              {"error", nlohmann::json{{"code", -32600},
                                                       {"message", "invalid request"}}}};
      }
      try {
        return handler(message);
      } catch (const std::exception& ex) {
        if (!message.contains("id")) {
          return nlohmann::json();
        }
        return nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", message["id"]},
                              {"error", nlohmann::json{{"code", -32603},
                                                       {"message", ex.what()}}}};
      }
    };

    if (parsed.is_array()) {
      nlohmann::json responses = nlohmann::json::array();
      for (const auto& message : parsed) {
        auto response = dispatch(message);
        if (!response.is_null()) {
          responses.push_back(std::move(response));
        }
      }
      if (responses.empty()) {
        respond(http::status::accepted, "", "application/json");
      } else {
        respond(http::status::ok, responses.dump(), "application/json");
      }
      return;
    }
    auto response = dispatch(parsed);
    if (response.is_null()) {
      respond(http::status::accepted, "", "application/json");
      return;
    }
    respond(http::status::ok, response.dump(), "application/json");
  }
};

HttpServer::HttpServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HttpServer::~HttpServer() { stop(); }

Result<std::unique_ptr<HttpServer>> HttpServer::start(std::string host,
                                                      std::uint16_t port,
                                                      Handler handler) {
  try {
    auto impl = std::make_unique<Impl>(std::move(handler));
    tcp::endpoint endpoint{net::ip::make_address(host), port};
    impl->acceptor.open(endpoint.protocol());
    impl->acceptor.set_option(net::socket_base::reuse_address(true));
    impl->acceptor.bind(endpoint);
    impl->acceptor.listen(net::socket_base::max_listen_connections);
    impl->accept_next();
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->context.run(); });
    return std::unique_ptr<HttpServer>(new HttpServer(std::move(impl)));
  } catch (const std::exception& ex) {
    return fail("MCP_HTTP_BIND_FAILED",
                "cannot bind " + host + ":" + std::to_string(port) + ": " +
                    ex.what());
  }
}

std::uint16_t HttpServer::port() const {
  beast::error_code ec;
  const auto endpoint = impl_->acceptor.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

void HttpServer::stop() {
  if (!impl_ || impl_->stopping.exchange(true)) {
    return;
  }
  beast::error_code ec;
  impl_->acceptor.close(ec);
  impl_->context.stop();
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

}  // namespace cf::mcp
