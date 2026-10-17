// Unit tests for the opencode server process (design sections 5.3, 6.6).
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cf/orch/opencode_server.hpp"

namespace {

using cf::orch::find_free_port;
using cf::orch::OpencodeServerOptions;
using cf::orch::OpencodeServerProcess;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_opencode_server_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_executable(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
  out.close();
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace);
}

const char* kFakeOpencode = R"python(#!/usr/bin/env python3
import argparse, http.server, json
parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("--hostname", default="127.0.0.1")
parser.add_argument("--port", type=int, required=True)
args = parser.parse_args()

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/global/health":
            body = json.dumps({"healthy": True, "version": "1.18.35"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()
    def log_message(self, *a):
        pass

print("fake opencode listening", flush=True)
http.server.HTTPServer((args.hostname, args.port), Handler).serve_forever()
)python";

class OpencodeServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    fake_binary_ = dir_ / "fake-opencode.py";
    write_executable(fake_binary_, kFakeOpencode);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  cf::agents::OpencodeClient make_client(uint16_t port) {
    cf::agents::OpencodeClientOptions options;
    options.host = "127.0.0.1";
    options.port = port;
    options.connect_timeout = std::chrono::milliseconds(500);
    options.request_timeout = std::chrono::milliseconds(2000);
    return cf::agents::OpencodeClient(options);
  }

  std::filesystem::path dir_;
  std::filesystem::path fake_binary_;
};

TEST_F(OpencodeServerTest, FindFreePortReturnsAListenablePort) {
  const uint16_t port = find_free_port();
  EXPECT_GT(port, 0);
}

TEST_F(OpencodeServerTest, StartsWaitsForHealthAndStops) {
  const uint16_t port = find_free_port();
  auto client = make_client(port);
  OpencodeServerOptions options;
  options.binary = fake_binary_.string();
  options.port = port;
  options.log_path = dir_ / "server.log";
  options.start_timeout = std::chrono::seconds(10);

  auto started = OpencodeServerProcess::start(options, client);
  ASSERT_TRUE(started.has_value()) << started.error().message;
  EXPECT_TRUE((*started)->running());
  EXPECT_EQ((*started)->port(), port);
  auto health = client.health();
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->value("version", ""), "1.18.35");

  (*started)->stop();
  EXPECT_FALSE((*started)->running());

  std::ifstream log(options.log_path);
  const std::string text((std::istreambuf_iterator<char>(log)),
                         std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("fake opencode listening"), std::string::npos);
}

TEST_F(OpencodeServerTest, MissingBinaryFailsCleanly) {
  const uint16_t port = find_free_port();
  auto client = make_client(port);
  OpencodeServerOptions options;
  options.binary = (dir_ / "does-not-exist").string();
  options.port = port;
  options.log_path = dir_ / "missing.log";
  options.start_timeout = std::chrono::seconds(5);

  auto started = OpencodeServerProcess::start(options, client);
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error().code, "ORCH_SERVER_EXITED");
}

TEST_F(OpencodeServerTest, HealthTimeoutKillsTheProcess) {
  const auto sleeper = dir_ / "sleeper.sh";
  write_executable(sleeper, "#!/bin/sh\nsleep 30\n");
  const uint16_t port = find_free_port();
  auto client = make_client(port);
  OpencodeServerOptions options;
  options.binary = sleeper.string();
  options.port = port;
  options.log_path = dir_ / "sleeper.log";
  options.start_timeout = std::chrono::milliseconds(600);
  options.stop_timeout = std::chrono::seconds(5);

  auto started = OpencodeServerProcess::start(options, client);
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error().code, "ORCH_SERVER_START_TIMEOUT");
}

// Guardrail: a server whose owning process dies must not survive as an
// orphan. An orphaned `opencode serve` used to be cleaned up by name, and
// that also matches the user's own session (both are named `opencode`).
TEST_F(OpencodeServerTest, ServerDiesWithItsParentProcess) {
  const uint16_t port = find_free_port();
  const auto pid_file = dir_ / "parent-death.pid";
  const pid_t parent = ::fork();
  ASSERT_GE(parent, 0);
  if (parent == 0) {
    // Child: spawn the server and exit without stopping it, so only the
    // kernel's parent-death signal can take it down.
    auto client = make_client(port);
    OpencodeServerOptions options;
    options.binary = fake_binary_.string();
    options.port = port;
    options.log_path = dir_ / "parent-death.log";
    options.start_timeout = std::chrono::seconds(10);
    auto started = OpencodeServerProcess::start(options, client);
    if (!started) {
      ::_exit(2);
    }
    std::ofstream out(pid_file);
    out << (*started)->pid();
    out.close();
    ::_exit(0);  // no destructor runs, so stop() is never called
  }

  int status = 0;
  ASSERT_EQ(::waitpid(parent, &status, 0), parent);

  pid_t server = 0;
  for (int i = 0; i < 100 && server == 0; ++i) {
    std::ifstream in(pid_file);
    if (in) {
      in >> server;
    }
    if (server == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_GT(server, 0);

  bool gone = false;
  for (int i = 0; i < 200 && !gone; ++i) {
    if (::kill(server, 0) != 0 && errno == ESRCH) {
      gone = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  EXPECT_TRUE(gone) << "opencode server " << server << " survived its parent";
}

TEST_F(OpencodeServerTest, DestructorStopsTheServer) {
  const uint16_t port = find_free_port();
  auto client = make_client(port);
  OpencodeServerOptions options;
  options.binary = fake_binary_.string();
  options.port = port;
  options.log_path = dir_ / "destroy.log";
  options.start_timeout = std::chrono::seconds(10);

  auto started = OpencodeServerProcess::start(options, client);
  ASSERT_TRUE(started.has_value()) << started.error().message;
  const pid_t pid = (*started)->pid();
  ASSERT_GT(pid, 0);
  (*started).reset();  // destructor stops the process
  EXPECT_EQ(::kill(pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

}  // namespace
