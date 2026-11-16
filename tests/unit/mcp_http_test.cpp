// Unit tests for the cf-mcp streamable-HTTP transport (design section 6.4).
#include <gtest/gtest.h>

#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "cf/mcp/http_server.hpp"

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

std::pair<unsigned, std::string> post(std::uint16_t port, const std::string& body) {
  net::io_context context;
  tcp::resolver resolver(context);
  beast::tcp_stream stream(context);
  const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
  stream.connect(endpoints);
  http::request<http::string_body> request{http::verb::post, "/mcp", 11};
  request.set(http::field::host, "127.0.0.1");
  request.set(http::field::content_type, "application/json");
  request.body() = body;
  request.prepare_payload();
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(stream, buffer, response);
  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  return {response.result_int(), response.body()};
}

TEST(McpHttpTest, AnswersRequestsAndNotifications) {
  auto started = cf::mcp::HttpServer::start(
      "127.0.0.1", 0, [](const nlohmann::json& request) -> nlohmann::json {
        if (!request.contains("id")) {
          return nlohmann::json();  // notification
        }
        return nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", request["id"]},
                              {"result", nlohmann::json{{"method",
                                                         request.value("method", "")}}}};
      });
  ASSERT_TRUE(started.has_value()) << started.error().message;
  const std::uint16_t port = (*started)->port();
  ASSERT_NE(port, 0);

  const auto single = post(port,
                           R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
  EXPECT_EQ(single.first, 200u);
  const auto document = nlohmann::json::parse(single.second);
  EXPECT_EQ(document["id"], 1);
  EXPECT_EQ(document["result"]["method"], "initialize");

  const auto batch = post(
      port,
      R"([{"jsonrpc":"2.0","id":2,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"}])");
  EXPECT_EQ(batch.first, 200u);
  const auto batch_document = nlohmann::json::parse(batch.second);
  ASSERT_TRUE(batch_document.is_array());
  ASSERT_EQ(batch_document.size(), 1u);
  EXPECT_EQ(batch_document[0]["id"], 2);

  const auto notification =
      post(port, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  EXPECT_EQ(notification.first, 202u);

  (*started)->stop();
}

}  // namespace
