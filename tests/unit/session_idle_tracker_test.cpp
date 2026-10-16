// Unit tests for event-driven turn completion (design sections 6.6.1, 6.6.2).
//
// The tracker records `session.idle` events; the driver waits on them instead
// of polling /session/status, which is empty between agent steps while tools
// run. The driver test uses a fake SSE server whose status endpoint always
// reports "busy", so only the event path can succeed.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

#include "cf/orch/opencode_session_driver.hpp"
#include "cf/orch/opencode_server.hpp"
#include "cf/orch/session_idle_tracker.hpp"

// GCC's libtsan before GCC 13 does not intercept pthread_cond_clockwait,
// which libstdc++ uses for timed condition_variable waits on glibc >= 2.30.
// Cross-thread wakeups through wait_for_idle are then reported as false
// "double lock"/"data race" warnings and the test binary exits non-zero.
// Clang's TSan and GCC 13+ intercept it, so only the older GCC runtime skips.
#if defined(__SANITIZE_THREAD__) && defined(__GNUC__) && !defined(__clang__) && \
    __GNUC__ < 13
#define CF_TSAN_NO_COND_CLOCKWAIT 1
#endif

namespace {

using cf::agents::OpencodeEvent;
using cf::orch::OpencodeSessionDriver;
using cf::orch::SessionIdleTracker;

OpencodeEvent idle_event(const std::string& session_id) {
  OpencodeEvent event;
  event.type = "session.idle";
  event.properties = cf::agents::Json{{"sessionID", session_id}};
  return event;
}

TEST(SessionIdleTrackerTest, CountsIdleEventsPerSession) {
  SessionIdleTracker tracker;
  EXPECT_EQ(tracker.idle_count("ses_a"), 0u);

  OpencodeEvent other;
  other.type = "message.updated";
  other.properties = cf::agents::Json{{"sessionID", "ses_a"}};
  EXPECT_FALSE(tracker.record(other));

  EXPECT_TRUE(tracker.record(idle_event("ses_a")));
  EXPECT_TRUE(tracker.record(idle_event("ses_a")));
  EXPECT_TRUE(tracker.record(idle_event("ses_b")));
  EXPECT_EQ(tracker.idle_count("ses_a"), 2u);
  EXPECT_EQ(tracker.idle_count("ses_b"), 1u);

  // An event without a session id is ignored.
  OpencodeEvent unnamed;
  unnamed.type = "session.idle";
  EXPECT_FALSE(tracker.record(unnamed));
}

TEST(SessionIdleTrackerTest, WaitReturnsWhenTheCountAdvances) {
#ifdef CF_TSAN_NO_COND_CLOCKWAIT
  GTEST_SKIP() << "GCC libtsan does not intercept pthread_cond_clockwait";
#endif
  SessionIdleTracker tracker;
  ASSERT_TRUE(tracker.record(idle_event("ses_a")));
  const std::uint64_t baseline = tracker.idle_count("ses_a");

  std::atomic<bool> woke{false};
  std::thread waiter([&] {
    woke.store(tracker.wait_for_idle("ses_a", baseline,
                                     std::chrono::milliseconds(2000)));
  });

  // An idle event for another session must not wake this waiter.
  ASSERT_TRUE(tracker.record(idle_event("ses_b")));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(woke.load());

  ASSERT_TRUE(tracker.record(idle_event("ses_a")));
  waiter.join();
  EXPECT_TRUE(woke.load());
}

TEST(SessionIdleTrackerTest, WaitTimesOutWithoutANewEvent) {
  SessionIdleTracker tracker;
  ASSERT_TRUE(tracker.record(idle_event("ses_a")));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(tracker.wait_for_idle("ses_a", tracker.idle_count("ses_a"),
                                     std::chrono::milliseconds(100)));
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(90));
}

const char* kFakeSseServer = R"python(#!/usr/bin/env python3
import argparse, http.server, json, threading, time
parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("--hostname", default="127.0.0.1")
parser.add_argument("--port", type=int, required=True)
args = parser.parse_args()

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/global/health":
            return self._json(200, {"healthy": True, "version": "1.18.35"})
        if self.path == "/event":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            time.sleep(0.3)  # let the client install its callback first
            event = b'data: {"type":"session.idle","properties":{"sessionID":"ses_test"}}\n\n'
            self.wfile.write(("%x\r\n" % len(event)).encode() + event + b"\r\n")
            self.wfile.flush()
            time.sleep(30)  # keep the stream open
            return
        if self.path == "/session/status":
            # Always busy: polling must never report the turn as finished.
            return self._json(200, {"ses_test": {"type": "busy"}})
        return self._json(404, {})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        if self.path == "/session":
            return self._json(200, {"id": "ses_test"})
        if self.path.endswith("/prompt_async"):
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        return self._json(200, {})

    def log_message(self, *a):
        pass

print("fake sse server ready", flush=True)
http.server.ThreadingHTTPServer((args.hostname, args.port), Handler).serve_forever()
)python";

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_idle_tracker_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

TEST(OpencodeSessionDriverTest, WaitsForTheIdleEventNotTheStatusEndpoint) {
#ifdef CF_TSAN_NO_COND_CLOCKWAIT
  GTEST_SKIP() << "GCC libtsan does not intercept pthread_cond_clockwait";
#endif
  const auto dir = make_temp_dir();
  const auto fake_binary = dir / "fake-sse.py";
  {
    std::ofstream out(fake_binary, std::ios::binary);
    out << kFakeSseServer;
  }
  std::filesystem::permissions(fake_binary,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace);

  const uint16_t port = cf::orch::find_free_port();
  cf::agents::OpencodeClientOptions client_options;
  client_options.host = "127.0.0.1";
  client_options.port = port;
  client_options.connect_timeout = std::chrono::milliseconds(1000);
  client_options.request_timeout = std::chrono::milliseconds(2000);
  cf::agents::OpencodeClient client(client_options);

  cf::orch::OpencodeServerOptions server_options;
  server_options.binary = fake_binary.string();
  server_options.port = port;
  server_options.log_path = dir / "server.log";
  server_options.start_timeout = std::chrono::seconds(10);
  auto server = cf::orch::OpencodeServerProcess::start(server_options, client);
  ASSERT_TRUE(server.has_value()) << server.error().message;

  auto stream = client.open_event_stream();
  ASSERT_TRUE(stream->wait_running(std::chrono::milliseconds(2000)));

  SessionIdleTracker tracker;
  stream->set_callback([&tracker](const OpencodeEvent& event) {
    (void)tracker.record(event);
  });
  OpencodeSessionDriver driver(client, &tracker, stream.get());

  auto session = driver.create_session();
  ASSERT_TRUE(session.has_value()) << session.error().message;
  ASSERT_EQ(*session, "ses_test");

  // The first turn's idle event arrives before wait_idle runs; the baseline
  // captured at send_prompt makes it count for this turn.
  ASSERT_TRUE(driver.send_prompt(*session, "do the work").has_value());
  EXPECT_TRUE(driver.wait_idle(*session, std::chrono::milliseconds(2000)).has_value());

  // No new idle event: the second turn times out even though the fake status
  // endpoint always says "busy" (polling is not consulted while the stream is
  // healthy).
  ASSERT_TRUE(driver.send_prompt(*session, "again").has_value());
  const auto timed_out = driver.wait_idle(*session, std::chrono::milliseconds(300));
  EXPECT_FALSE(timed_out.has_value());
  EXPECT_EQ(timed_out.error().code, cf::agents::error_codes::kTimeout);

  stream->stop();
  (*server)->stop();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
