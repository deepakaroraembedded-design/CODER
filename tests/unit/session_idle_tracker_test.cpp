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
  EXPECT_FALSE(tracker.last_activity("ses_a").has_value());

  OpencodeEvent other;
  other.type = "message.updated";
  other.properties = cf::agents::Json{{"sessionID", "ses_a"}};
  EXPECT_FALSE(tracker.record(other));
  // Any session event counts as activity, not only idle ones (it drives the
  // no-events stall watchdog, design section 6.6.1 layer 3).
  ASSERT_TRUE(tracker.last_activity("ses_a").has_value());

  EXPECT_TRUE(tracker.record(idle_event("ses_a")));
  EXPECT_TRUE(tracker.record(idle_event("ses_a")));
  EXPECT_TRUE(tracker.record(idle_event("ses_b")));
  EXPECT_EQ(tracker.idle_count("ses_a"), 2u);
  EXPECT_EQ(tracker.idle_count("ses_b"), 1u);
  EXPECT_FALSE(tracker.last_activity("ses_c").has_value());

  // An event without a session id is ignored.
  OpencodeEvent unnamed;
  unnamed.type = "session.idle";
  EXPECT_FALSE(tracker.record(unnamed));
}

TEST(SessionIdleTrackerTest, TracksRunningToolCalls) {
  SessionIdleTracker tracker;
  OpencodeEvent start;
  start.type = "message.part.updated";
  start.properties = cf::agents::Json{
      {"sessionID", "ses_a"},
      {"part", cf::agents::Json{{"type", "tool"},
                                {"state", cf::agents::Json{{"status", "running"}}}}}};
  EXPECT_FALSE(tracker.record(start));
  EXPECT_TRUE(tracker.in_tool("ses_a"));
  EXPECT_FALSE(tracker.in_tool("ses_b"));

  OpencodeEvent finish = start;
  finish.properties["part"]["state"]["status"] = "completed";
  EXPECT_FALSE(tracker.record(finish));
  EXPECT_FALSE(tracker.in_tool("ses_a"));

  // Other event types leave the tool state alone (deltas can arrive while a
  // tool runs in parallel).
  (void)tracker.record(start);
  OpencodeEvent delta;
  delta.type = "message.part.delta";
  delta.properties = cf::agents::Json{{"sessionID", "ses_a"}, {"delta", "x"}};
  EXPECT_FALSE(tracker.record(delta));
  EXPECT_TRUE(tracker.in_tool("ses_a"));
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

// A stream that reports itself running; the test drives the tracker directly.
class FakeStream final : public cf::agents::EventStream {
 public:
  void set_callback(Callback) override {}
  void set_disconnect_callback(DisconnectCallback) override {}
  void stop() override {}
  bool running() const override { return true; }
  bool wait_running(std::chrono::milliseconds) override { return true; }
};

TEST(OpencodeSessionDriverTest, HealthFollowsTheServerCallback) {
  FakeStream stream;
  SessionIdleTracker tracker;
  cf::agents::OpencodeClientOptions client_options;
  cf::agents::OpencodeClient client(client_options);
  bool healthy = true;
  OpencodeSessionDriver driver(client, &tracker, &stream,
                               [&] { return healthy; });
  EXPECT_TRUE(driver.healthy());
  healthy = false;
  EXPECT_FALSE(driver.healthy());

  // Without a callback the driver reports healthy (tests, fakes).
  OpencodeSessionDriver always(client);
  EXPECT_TRUE(always.healthy());
}

TEST(OpencodeSessionDriverTest, RunningToolCallIsNotAStall) {
#ifdef CF_TSAN_NO_COND_CLOCKWAIT
  GTEST_SKIP() << "GCC libtsan does not intercept pthread_cond_clockwait";
#endif
  FakeStream stream;
  SessionIdleTracker tracker;
  cf::agents::OpencodeClientOptions client_options;
  cf::agents::OpencodeClient client(client_options);
  OpencodeSessionDriver driver(client, &tracker, &stream);

  // The coder's long build: the tool starts mid-wait, then emits nothing.
  OpencodeEvent tool_start;
  tool_start.type = "message.part.updated";
  tool_start.properties = cf::agents::Json{
      {"sessionID", "ses_a"},
      {"part", cf::agents::Json{{"type", "tool"},
                                {"state", cf::agents::Json{{"status", "running"}}}}}};
  std::thread recorder([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)tracker.record(tool_start);
  });

  const auto started = std::chrono::steady_clock::now();
  const auto status = driver.wait_idle("ses_a", std::chrono::milliseconds(400),
                                       std::chrono::milliseconds(100));
  recorder.join();
  EXPECT_FALSE(status.has_value());
  // The overall deadline ended the wait, not the stall watchdog: a running
  // tool is not a stalled provider stream.
  EXPECT_EQ(status.error().code, cf::agents::error_codes::kTimeout);
  EXPECT_EQ(status.error().message.find("stall watchdog"), std::string::npos);
  EXPECT_GE(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(350));
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
  EXPECT_TRUE(driver.wait_idle(*session, std::chrono::milliseconds(2000),
                               std::chrono::milliseconds(120000))
                  .has_value());

  // No new idle event: with the stall watchdog on, the wait fails as soon as
  // the session has been quiet for the stall timeout, not at the overall
  // deadline (design section 6.6.1 layer 3: no events for 120 s).
  ASSERT_TRUE(driver.send_prompt(*session, "again").has_value());
  const auto stalled = driver.wait_idle(*session, std::chrono::milliseconds(5000),
                                        std::chrono::milliseconds(100));
  EXPECT_FALSE(stalled.has_value());
  EXPECT_EQ(stalled.error().code, cf::agents::error_codes::kTimeout);
  EXPECT_NE(stalled.error().message.find("stall watchdog"), std::string::npos);

  // With the watchdog disabled the same wait runs to its deadline, even
  // though the fake status endpoint always says "busy" (polling is not
  // consulted while the stream is healthy).
  const auto timed_out = driver.wait_idle(*session, std::chrono::milliseconds(300),
                                          std::chrono::milliseconds(0));
  EXPECT_FALSE(timed_out.has_value());
  EXPECT_EQ(timed_out.error().code, cf::agents::error_codes::kTimeout);
  EXPECT_EQ(timed_out.error().message.find("stall watchdog"), std::string::npos);

  stream->stop();
  (*server)->stop();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
