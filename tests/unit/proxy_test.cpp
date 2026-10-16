// Unit tests for the egress allow-listing proxy (design section 6.8.2).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cf/sandbox/proxy.hpp"

namespace {

using cf::sandbox::EgressProxy;
using cf::sandbox::host_allowed;
using cf::sandbox::ProxyOptions;

// A one-shot TCP server that replies "hello" after receiving anything.
class EchoServer {
 public:
  EchoServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::listen(fd_, 4);
    socklen_t length = sizeof(address);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client >= 0) {
        char buffer[64];
        (void)::recv(client, buffer, sizeof(buffer), 0);
        const char* reply = "hello";
        (void)::send(client, reply, 5, 0);
        ::close(client);
      }
    });
  }

  ~EchoServer() {
    ::close(fd_);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  uint16_t port() const { return port_; }

 private:
  int fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

std::string read_response(int fd) {
  std::string response;
  char buffer[512];
  const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
  if (got > 0) {
    response.assign(buffer, static_cast<std::size_t>(got));
  }
  return response;
}

int connect_to_proxy(uint16_t proxy_port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(proxy_port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

TEST(EgressProxyTest, HostMatcherHandlesPatterns) {
  const std::vector<std::string> patterns = {"api.fireworks.ai:443",
                                             "*.example.com", "127.0.0.1"};
  EXPECT_TRUE(host_allowed(patterns, "api.fireworks.ai", 443));
  EXPECT_FALSE(host_allowed(patterns, "api.fireworks.ai", 80));  // port pinned
  EXPECT_TRUE(host_allowed(patterns, "a.example.com", 8443));
  EXPECT_TRUE(host_allowed(patterns, "deep.a.example.com", 443));
  EXPECT_FALSE(host_allowed(patterns, "example.com", 443));  // suffix needs a dot
  EXPECT_FALSE(host_allowed(patterns, "evil.com", 443));
  EXPECT_TRUE(host_allowed(patterns, "127.0.0.1", 1234));  // any port
}

TEST(EgressProxyTest, TunnelsToAllowListedHosts) {
  EchoServer echo;
  ProxyOptions options;
  options.allowed_hosts = {"127.0.0.1:" + std::to_string(echo.port())};
  auto proxy = EgressProxy::start(options);
  ASSERT_TRUE(proxy.has_value()) << proxy.error().message;

  const int client = connect_to_proxy((*proxy)->port());
  ASSERT_GE(client, 0);
  const std::string request = "CONNECT 127.0.0.1:" + std::to_string(echo.port()) +
                              " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_GT(::send(client, request.data(), request.size(), 0), 0);
  const std::string response = read_response(client);
  EXPECT_NE(response.find("200 Connection Established"), std::string::npos);
  ASSERT_GT(::send(client, "ping", 4, 0), 0);
  const std::string echoed = read_response(client);
  EXPECT_EQ(echoed, "hello");
  ::close(client);

  EXPECT_EQ((*proxy)->allowed_connections(), 1u);
  EXPECT_EQ((*proxy)->denied_requests(), 0u);
}

TEST(EgressProxyTest, DeniesHostsOutsideTheAllowList) {
  ProxyOptions options;
  options.allowed_hosts = {"api.fireworks.ai:443"};
  auto proxy = EgressProxy::start(options);
  ASSERT_TRUE(proxy.has_value()) << proxy.error().message;

  const int client = connect_to_proxy((*proxy)->port());
  ASSERT_GE(client, 0);
  const std::string request = "CONNECT evil.example.com:443 HTTP/1.1\r\n\r\n";
  ASSERT_GT(::send(client, request.data(), request.size(), 0), 0);
  const std::string response = read_response(client);
  EXPECT_NE(response.find("403 Forbidden"), std::string::npos);
  ::close(client);

  EXPECT_EQ((*proxy)->allowed_connections(), 0u);
  EXPECT_EQ((*proxy)->denied_requests(), 1u);
}

TEST(EgressProxyTest, RejectsNonConnectMethods) {
  ProxyOptions options;
  options.allowed_hosts = {"api.fireworks.ai:443"};
  auto proxy = EgressProxy::start(options);
  ASSERT_TRUE(proxy.has_value()) << proxy.error().message;

  const int client = connect_to_proxy((*proxy)->port());
  ASSERT_GE(client, 0);
  const std::string request = "GET http://api.fireworks.ai/ HTTP/1.1\r\n\r\n";
  ASSERT_GT(::send(client, request.data(), request.size(), 0), 0);
  const std::string response = read_response(client);
  EXPECT_NE(response.find("405"), std::string::npos);
  ::close(client);
  EXPECT_EQ((*proxy)->denied_requests(), 1u);
}

TEST(EgressProxyTest, StopIsIdempotent) {
  ProxyOptions options;
  options.allowed_hosts = {"api.fireworks.ai"};
  auto proxy = EgressProxy::start(options);
  ASSERT_TRUE(proxy.has_value());
  (*proxy)->stop();
  (*proxy)->stop();
  EXPECT_EQ((*proxy)->allowed_connections(), 0u);
}

}  // namespace
