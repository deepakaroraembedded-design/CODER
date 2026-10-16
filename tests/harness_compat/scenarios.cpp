// Phase 0a harness compatibility scenarios H1-H15 (design section 6.6.2).
#include "harness.hpp"

#include <cstring>
#include <fstream>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace cf::spike {

namespace {

using namespace std::chrono_literals;

std::chrono::milliseconds budget(const SpikeOptions& options) {
  return std::chrono::seconds(options.timeout_s);
}

void send_prompt(ScenarioContext& ctx, const std::string& session_id,
                 const std::string& text) {
  auto status = ctx.prompt(session_id, text);
  if (!status) {
    spdlog::warn("prompt failed for {}: {}", session_id, status.error().message);
  }
}

void send_prompt_with_model(ScenarioContext& ctx, const std::string& session_id,
                            const std::string& text, const std::string& provider_id,
                            const std::string& model_id) {
  auto status = ctx.prompt_with_model(session_id, text, provider_id, model_id);
  if (!status) {
    spdlog::warn("prompt failed for {}: {}", session_id, status.error().message);
  }
}

std::string tool_name(const Json& part) { return part.value("tool", ""); }

std::string tool_status(const Json& part) {
  return part.value("state", Json::object()).value("status", "");
}

Json summarize_tools(const std::vector<Json>& parts) {
  Json out = Json::array();
  for (const auto& part : parts) {
    out.push_back(Json{{"tool", tool_name(part)}, {"status", tool_status(part)}});
  }
  return out;
}

bool text_contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

IterationResult make_result(const std::string& scenario, int iteration, bool pass,
                            std::string detail) {
  IterationResult result;
  result.scenario = scenario;
  result.iteration = iteration;
  result.pass = pass;
  result.detail = std::move(detail);
  return result;
}

IterationResult setup_failed(const std::string& scenario, int iteration,
                             const Status& status) {
  IterationResult result =
      make_result(scenario, iteration, false, "setup failed: " + status.error().message);
  result.evidence["error_code"] = status.error().code;
  return result;
}

// A minimal OpenAI-compatible provider used for scenario H11.
class MockProvider {
 public:
  enum class Mode { Always429, FailTwiceThenSucceed, Hang };

  ~MockProvider() { stop(); }

  Status start(Mode mode) {
    mode_ = mode;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return fail("SPIKE_MOCK_SOCKET", std::strerror(errno));
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return fail("SPIKE_MOCK_BIND", std::strerror(errno));
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
    return {};
  }

  void stop() {
    stop_.store(true);
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  uint16_t port() const { return port_; }
  int requests() const { return requests_.load(); }

 private:
  void run() {
    while (!stop_.load()) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        break;
      }
      handle(fd);
      ::close(fd);
    }
  }

  static void write_all(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
      if (written <= 0) {
        return;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  void handle(int fd) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = ::read(fd, buffer, sizeof(buffer));
      if (n <= 0) {
        return;
      }
      request.append(buffer, static_cast<std::size_t>(n));
      if (request.size() > 65536) {
        return;
      }
    }
    if (request.rfind("GET ", 0) == 0) {
      const std::string body =
          R"({"object":"list","data":[{"id":"mock-model","object":"model"}]})";
      write_all(fd, "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\n"
                    "content-length: " + std::to_string(body.size()) +
                        "\r\nconnection: close\r\n\r\n" + body);
      return;
    }

    const int count = ++requests_;
    if (mode_ == Mode::Hang) {
      while (!stop_.load()) {
        std::this_thread::sleep_for(200ms);
      }
      return;
    }
    if (mode_ == Mode::Always429 ||
        (mode_ == Mode::FailTwiceThenSucceed && count <= 2)) {
      const std::string body = R"({"error":{"message":"mock rate limit","type":"rate_limit_error"}})";
      write_all(fd, "HTTP/1.1 429 Too Many Requests\r\n"
                    "content-type: application/json\r\nretry-after: 1\r\n"
                    "content-length: " + std::to_string(body.size()) +
                        "\r\nconnection: close\r\n\r\n" + body);
      return;
    }

    Json first = Json{{"id", "chatcmpl-mock"},
                      {"object", "chat.completion.chunk"},
                      {"created", 0},
                      {"model", "mock-model"},
                      {"choices", Json::array({Json{{"index", 0},
                                                   {"delta", Json{{"role", "assistant"},
                                                                  {"content", "MOCK-OK"}}},
                                                   {"finish_reason", nullptr}}})}};
    Json last = Json{{"id", "chatcmpl-mock"},
                     {"object", "chat.completion.chunk"},
                     {"created", 0},
                     {"model", "mock-model"},
                     {"choices", Json::array({Json{{"index", 0},
                                                   {"delta", Json::object()},
                                                   {"finish_reason", "stop"}}})}};
    const std::string body = "data: " + first.dump() + "\n\ndata: " + last.dump() +
                             "\n\ndata: [DONE]\n\n";
    write_all(fd, "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\n"
                  "cache-control: no-cache\r\nconnection: close\r\n\r\n" + body);
  }

  Mode mode_ = Mode::Always429;
  std::atomic<bool> stop_{false};
  std::atomic<int> requests_{0};
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

Json mock_provider_config(uint16_t port) {
  return Json{{"provider",
               Json{{"spike-mock",
                     Json{{"npm", "@ai-sdk/openai-compatible"},
                          {"name", "Spike Mock"},
                          {"options",
                           Json{{"baseURL", "http://127.0.0.1:" + std::to_string(port) + "/v1"},
                                {"apiKey", "spike"}}},
                          {"models",
                           Json{{"mock-model",
                                 Json{{"name", "Mock Model"}, {"tool_call", false}}}}}}}}}};
}

}  // namespace

// H1: start a session, send a task, stream events to completion.
IterationResult run_h1(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H1", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H1", iteration, status);
  }
  auto session = ctx.create_session();
  if (!session) {
    return make_result("H1", iteration, false, "session create failed");
  }
  const std::string sid = *session;

  if (auto status = ctx.prompt(sid,
                               "Reply with exactly the word PONG and nothing else. "
                               "Do not use any tools.");
      !status) {
    return make_result("H1", iteration, false, "prompt failed: " + status.error().message);
  }

  const auto start = std::chrono::steady_clock::now();
  const bool idle = ctx.wait_session_idle(sid, budget(options));
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  const std::string text = ctx.last_assistant_text(sid);

  IterationResult result = make_result(
      "H1", iteration, idle && text_contains(text, "PONG"),
      idle ? (text_contains(text, "PONG") ? "turn completed with PONG"
                                          : "idle but final text was: " + text)
           : "session did not become idle within budget");
  result.evidence["elapsed_ms"] = elapsed_ms;
  result.evidence["final_text"] = text;
  result.evidence["event_types"] = ctx.events().type_counts();
  result.evidence["stream_disconnects"] =
      ctx.events().of_type("spike.stream_disconnect").size();
  return result;
}

// H2: structured submission through an MCP tool.
IterationResult run_h2(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H2", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H2", iteration, status);
  }
  auto session = ctx.create_session();
  if (!session) {
    return make_result("H2", iteration, false, "session create failed");
  }
  const std::string sid = *session;

  if (auto status = ctx.prompt(
          sid,
          "Call the cf_submit_work tool exactly once with summary=\"spike-h2\" and "
          "defect_ids=[]. Do not call any other tool. After the tool succeeds, reply "
          "SUBMITTED.");
      !status) {
    return make_result("H2", iteration, false, "prompt failed");
  }

  const bool idle = ctx.wait_session_idle(sid, budget(options));
  const auto calls = ctx.mcp_calls();
  bool submit_ok = false;
  for (const auto& call : calls) {
    if (call.value("tool", "") == "submit_work" &&
        call.value("arguments", Json::object()).value("summary", "") == "spike-h2") {
      submit_ok = true;
    }
  }
  const auto tools = ctx.tool_parts(sid);

  IterationResult result =
      make_result("H2", iteration, idle && submit_ok,
                  idle ? (submit_ok ? "submit_work received with valid JSON"
                                    : "idle but no valid submit_work call recorded")
                       : "session did not become idle");
  result.evidence["mcp_calls"] = calls;
  result.evidence["tools"] = summarize_tools(tools);
  result.evidence["final_text"] = ctx.last_assistant_text(sid);
  return result;
}

// H3: permission prompt answered by the broker (once and reject).
IterationResult run_h3(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H3", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H3", iteration, status);
  }
  Json evidence = Json::object();

  // Sub-case a: reply "once"; the command must run and the turn must continue.
  auto session_a = ctx.create_session();
  if (!session_a) {
    return make_result("H3", iteration, false, "session create failed");
  }
  send_prompt(ctx, *session_a,
             "Run the bash command: echo hello-once. Then reply ONCE-DONE.");
  const bool idle_a = ctx.wait_session_idle(*session_a, budget(options));
  const auto tools_a = ctx.tool_parts(*session_a);
  bool bash_ok = false;
  for (const auto& part : tools_a) {
    if (tool_name(part).find("bash") != std::string::npos &&
        tool_status(part) == "completed") {
      bash_ok = true;
    }
  }
  const std::string text_a = ctx.last_assistant_text(*session_a);
  const int answered_a = ctx.permissions_answered();
  evidence["once"] = Json{{"idle", idle_a},
                          {"permission_requests", ctx.permission_requests().size()},
                          {"permissions_answered", answered_a},
                          {"bash_completed", bash_ok},
                          {"final_text", text_a},
                          {"tools", summarize_tools(tools_a)}};

  // Sub-case b: reply "reject"; the turn must continue cleanly without the command.
  ctx.set_permission_policy([](const agents::OpencodeEvent&) { return "reject"; });
  auto session_b = ctx.create_session();
  if (!session_b) {
    return make_result("H3", iteration, false, "session create failed (reject)");
  }
  send_prompt(ctx, *session_b,
             "Run the bash command: echo should-not-run. Then reply REJECT-DONE.");
  const bool idle_b = ctx.wait_session_idle(*session_b, budget(options));
  const auto tools_b = ctx.tool_parts(*session_b);
  bool bash_completed_b = false;
  for (const auto& part : tools_b) {
    if (tool_name(part).find("bash") != std::string::npos &&
        tool_status(part) == "completed") {
      bash_completed_b = true;
    }
  }
  const std::string text_b = ctx.last_assistant_text(*session_b);
  evidence["reject"] = Json{{"idle", idle_b},
                            {"bash_completed", bash_completed_b},
                            {"final_text", text_b},
                            {"tools", summarize_tools(tools_b)}};

  // No "always" reply is ever sent by the broker.
  bool always_sent = false;
  for (const auto& event : ctx.events().of_type("permission.replied")) {
    if (event.properties.value("response", "") == "always") {
      always_sent = true;
    }
  }
  evidence["always_replies_sent"] = always_sent;

  const bool pass = idle_a && idle_b && bash_ok && !bash_completed_b && !always_sent;
  IterationResult result =
      make_result("H3", iteration, pass,
                  pass ? "once executed, reject blocked, no always grants"
                       : "see evidence for sub-case outcomes");
  result.evidence = std::move(evidence);
  return result;
}

// H4: every permission type the pinned version can raise is detected and answered.
IterationResult run_h4(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H4", iteration);
  const Json overrides = Json{
      {"permission",
       Json{{"edit", "ask"},
            {"read", "ask"},
            {"glob", "ask"},
            {"grep", "ask"},
            {"list", "ask"},
            {"bash", Json{{"*", "ask"}}},
            {"webfetch", "ask"},
            {"task", "ask"},
            {"external_directory", "ask"},
            {"lsp", "ask"}}},
  };
  if (auto status = ctx.setup(overrides); !status) {
    return setup_failed("H4", iteration, status);
  }

  auto session = ctx.create_session();
  if (!session) {
    return make_result("H4", iteration, false, "session create failed");
  }
  const std::string sid = *session;
  send_prompt(ctx, sid,
             "Do all of the following, one at a time: (1) create a file named h4.txt "
             "containing the word hello, (2) read h4.txt back, (3) list the current "
             "directory, (4) search the directory for files matching *.txt, (5) search "
             "file contents for the word hello, (6) run the bash command: echo h4-bash. "
             "Then reply H4-DONE.");
  const bool idle = ctx.wait_session_idle(sid, budget(options));

  const auto requests = ctx.permission_requests();
  Json observed = Json::array();
  for (const auto& request : requests) {
    observed.push_back(request.properties.value("permission", ""));
  }
  const auto tools = ctx.tool_parts(sid);

  IterationResult result = make_result(
      "H4", iteration, idle && ctx.permissions_answered() == static_cast<int>(requests.size()),
      idle ? "all observed permission requests answered" : "session did not become idle");
  result.evidence["observed_permission_types"] = observed;
  result.evidence["requests"] = requests.size();
  result.evidence["answered"] = ctx.permissions_answered();
  result.evidence["tools"] = summarize_tools(tools);
  result.evidence["final_text"] = ctx.last_assistant_text(sid);
  return result;
}

// H5: MCP tool error, timeout and invalid JSON are handled without a hang.
IterationResult run_h5(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H5", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H5", iteration, status);
  }
  Json evidence = Json::object();
  bool all_idle = true;

  const auto run_case = [&](const std::string& name, const std::string& prompt_text) {
    auto session = ctx.create_session();
    if (!session) {
      evidence[name] = Json{{"error", "session create failed"}};
      all_idle = false;
      return;
    }
    send_prompt(ctx, *session, prompt_text);
    const bool idle = ctx.wait_session_idle(*session, budget(options));
    all_idle = all_idle && idle;
    evidence[name] = Json{{"idle", idle},
                          {"tools", summarize_tools(ctx.tool_parts(*session))},
                          {"final_text", ctx.last_assistant_text(*session)}};
  };

  run_case("error",
           "Call the cf_spike_error tool. Then reply what happened.");
  run_case("timeout",
           "Call the cf_spike_timeout tool with seconds=15. Then reply what happened.");
  run_case("invalid_json",
           "Call the cf_spike_invalid_json tool. Then reply what happened.");

  const auto calls = ctx.mcp_calls();
  evidence["mcp_calls"] = calls;
  IterationResult result = make_result(
      "H5", iteration, all_idle,
      all_idle ? "all fault-injected MCP calls ended without a hang"
               : "one or more sessions did not become idle");
  result.evidence = std::move(evidence);
  return result;
}

// H6: malformed tool arguments produce a schema error and no hang.
IterationResult run_h6(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H6", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H6", iteration, status);
  }
  auto session = ctx.create_session();
  if (!session) {
    return make_result("H6", iteration, false, "session create failed");
  }
  const std::string sid = *session;
  send_prompt(ctx, sid,
             "Call the cf_spike_echo tool with the text argument set to the integer "
             "42 (a number, not a string). If the call fails, correct the arguments "
             "and try again. Then reply H6-DONE.");
  const bool idle = ctx.wait_session_idle(sid, budget(options));

  const auto calls = ctx.mcp_calls();
  bool recovered = false;
  for (const auto& call : calls) {
    if (call.value("tool", "") == "spike_echo" &&
        call.value("arguments", Json::object()).value("text", "") == "42") {
      recovered = true;
    }
  }
  const auto tools = ctx.tool_parts(sid);

  IterationResult result = make_result(
      "H6", iteration, idle,
      idle ? (recovered ? "schema error surfaced and model recovered with a valid call"
                        : "session ended cleanly; model did not retry with a string")
           : "session did not become idle");
  result.evidence["mcp_calls"] = calls;
  result.evidence["tools"] = summarize_tools(tools);
  result.evidence["recovered_with_valid_args"] = recovered;
  result.evidence["final_text"] = ctx.last_assistant_text(sid);
  return result;
}

// H7: event stream drops and reconnects; missed state is recovered by polling.
IterationResult run_h7(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H7", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H7", iteration, status);
  }
  auto session = ctx.create_session();
  if (!session) {
    return make_result("H7", iteration, false, "session create failed");
  }
  const std::string sid = *session;
  send_prompt(ctx, sid, "Run the bash command: sleep 4. Then reply RECOVERED-7.");

  // Drop the stream almost immediately so the permission.asked event is
  // missed entirely, then reconnect.
  std::this_thread::sleep_for(300ms);
  ctx.drop_stream();
  const auto before_count = ctx.events().snapshot().size();
  std::this_thread::sleep_for(2s);
  if (auto status = ctx.reopen_stream(); !status) {
    return make_result("H7", iteration, false, "stream reopen failed");
  }

  // State poll: find the pending permission request and answer it.
  std::string recovered_request;
  const auto poll_deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < poll_deadline && recovered_request.empty()) {
    auto pending = ctx.client().get("/permission");
    if (pending && pending->is_array()) {
      for (const auto& request : *pending) {
        if (request.value("sessionID", "") == sid) {
          recovered_request = request.value("id", "");
        }
      }
    }
    if (recovered_request.empty()) {
      std::this_thread::sleep_for(500ms);
    }
  }
  bool replied = false;
  if (!recovered_request.empty()) {
    replied = ctx.client().reply_permission(sid, recovered_request, "once").has_value();
  }

  const bool idle = ctx.wait_session_idle(sid, budget(options));
  const std::string text = ctx.last_assistant_text(sid);
  const auto after_count = ctx.events().snapshot().size();

  IterationResult result = make_result(
      "H7", iteration, idle && text_contains(text, "RECOVERED-7"),
      idle ? (text_contains(text, "RECOVERED-7")
                  ? "state recovered after stream drop via pending-permission poll"
                  : "idle but final text was: " + text)
           : "session did not become idle after reconnect");
  result.evidence["events_before_drop"] = before_count;
  result.evidence["events_after_reconnect"] = after_count;
  result.evidence["missed_permission_recovered"] = !recovered_request.empty();
  result.evidence["recovered_request_id"] = recovered_request;
  result.evidence["replied"] = replied;
  result.evidence["final_text"] = text;
  return result;
}

// H8: opencode server killed mid-turn; the adapter detects it within 10 s.
IterationResult run_h8(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H8", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H8", iteration, status);
  }
  auto session = ctx.create_session();
  if (!session) {
    return make_result("H8", iteration, false, "session create failed");
  }
  const std::string sid = *session;
  send_prompt(ctx, sid, "Run the bash command: sleep 30. Then reply SLOW-DONE.");

  const bool started = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" || event.type == "message.part.updated";
      },
      60s);
  if (!started) {
    return make_result("H8", iteration, false, "turn never started");
  }
  std::this_thread::sleep_for(2s);

  const auto kill_time = std::chrono::steady_clock::now();
  ctx.server().kill();
  const bool detected = ctx.events().wait_for(
      [](const agents::OpencodeEvent& event) {
        return event.type == "spike.stream_disconnect";
      },
      15s);
  const auto detection_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - kill_time)
                                .count();

  IterationResult result =
      make_result("H8", iteration, detected && detection_ms <= 10000,
                  detected ? "server death detected in " +
                                 std::to_string(detection_ms) + " ms"
                           : "server death not detected within 15 s");
  result.evidence["detection_ms"] = detection_ms;
  result.evidence["detected"] = detected;
  result.evidence["session_id"] = sid;
  return result;
}

// H9: watchdog cancel; no further file changes after the cancel.
IterationResult run_h9(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H9", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H9", iteration, status);
  }
  auto session = ctx.create_session();
  if (!session) {
    return make_result("H9", iteration, false, "session create failed");
  }
  const std::string sid = *session;
  send_prompt(ctx, sid,
             "Run the bash command: sleep 8 && echo LATE > late.txt. Then reply "
             "CANCEL-DONE.");

  const bool running = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" || event.type == "message.part.updated";
      },
      60s);
  if (!running) {
    return make_result("H9", iteration, false, "turn never started");
  }
  std::this_thread::sleep_for(1500ms);

  auto abort_status = ctx.client().interrupt(sid);
  const auto cancel_time = std::chrono::steady_clock::now();
  const bool idle = ctx.wait_session_idle(sid, 30s);
  const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - cancel_time)
                           .count();

  // The command sleeps 8 s; wait past that and check the file never appeared.
  std::this_thread::sleep_for(9s);
  const bool late_file = fs::exists(ctx.project_dir() / "late.txt");

  IterationResult result = make_result(
      "H9", iteration, abort_status.has_value() && !late_file,
      !late_file ? "cancel stopped the turn before the command completed"
                 : "late.txt was created after the cancel");
  result.evidence["abort_ok"] = abort_status.has_value();
  result.evidence["idle_after_cancel"] = idle;
  result.evidence["idle_after_cancel_ms"] = idle_ms;
  result.evidence["late_file_created"] = late_file;
  result.evidence["tools"] = summarize_tools(ctx.tool_parts(sid));
  return result;
}

// H10: the model asks a question instead of submitting.
IterationResult run_h10(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H10", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H10", iteration, status);
  }
  Json evidence = Json::object();

  // Sub-case a: the question tool; the broker answers the first option.
  ctx.set_question_policy([](const agents::OpencodeEvent&) { return "answer_first"; });
  auto session_a = ctx.create_session();
  if (!session_a) {
    return make_result("H10", iteration, false, "session create failed");
  }
  send_prompt(ctx, *session_a,
             "Use the question tool to ask the user: \"Should I proceed?\" with the "
             "options \"yes\" and \"no\". Do not call any other tool. After you receive "
             "the answer, reply QUESTION-ANSWERED.");
  const bool idle_a = ctx.wait_session_idle(*session_a, budget(options));
  const auto questions = ctx.question_requests();
  evidence["question_tool"] =
      Json{{"idle", idle_a},
           {"questions_asked", questions.size()},
           {"final_text", ctx.last_assistant_text(*session_a)}};

  // Sub-case b: the model ends the turn with a text question; the orchestrator
  // sends the fixed continuation message and the model finishes the task.
  auto session_b = ctx.create_session();
  if (!session_b) {
    return make_result("H10", iteration, false, "session create failed (b)");
  }
  send_prompt(ctx, *session_b,
              "Task: call the cf_submit_work tool with summary=\"h10b\" and "
              "defect_ids=[]. Before starting, ask me whether to proceed. Do not "
              "call any tools in this turn; just ask your question.");
  const bool idle_b1 = ctx.wait_session_idle(*session_b, budget(options));
  const auto mark_b = ctx.events().mark();
  send_prompt(ctx, *session_b,
              "Proceed with the task as specified. Do not ask questions. Finish by "
              "calling cf_submit_work with summary=\"h10b\" and defect_ids=[].");
  const bool idle_b2 = ctx.wait_session_idle(*session_b, budget(options), mark_b);
  bool submitted = false;
  for (const auto& call : ctx.mcp_calls()) {
    if (call.value("tool", "") == "submit_work" &&
        call.value("arguments", Json::object()).value("summary", "") == "h10b") {
      submitted = true;
    }
  }
  Json messages_summary = Json::array();
  if (auto messages = ctx.client().session_messages(*session_b);
      messages && messages->is_array()) {
    for (const auto& item : *messages) {
      if (!item.contains("info") || item["info"].value("role", "") != "assistant") {
        continue;
      }
      Json entry = Json{{"texts", Json::array()}, {"tools", Json::array()},
                        {"part_types", Json::array()}};
      if (item.contains("parts")) {
        for (const auto& part : item["parts"]) {
          entry["part_types"].push_back(part.value("type", ""));
          if (part.value("type", "") == "text") {
            entry["texts"].push_back(part.value("text", ""));
          } else if (part.value("type", "") == "tool") {
            entry["tools"].push_back(part.value("tool", ""));
          }
        }
      }
      messages_summary.push_back(entry);
    }
  }
  evidence["continuation"] = Json{{"first_turn_idle", idle_b1},
                                  {"second_turn_idle", idle_b2},
                                  {"submitted_after_continuation", submitted},
                                  {"final_text", ctx.last_assistant_text(*session_b)},
                                  {"assistant_messages", messages_summary}};

  const bool pass = idle_a && questions.size() >= 1 && idle_b1 && idle_b2 && submitted;
  IterationResult result =
      make_result("H10", iteration, pass,
                  pass ? "question tool answered; continuation completed the task"
                       : "see evidence for sub-case outcomes");
  result.evidence = std::move(evidence);
  return result;
}

// H11: provider errors (429, 5xx, timeout) are retried with backoff or fail cleanly.
IterationResult run_h11(const SpikeOptions& options, int iteration) {
  Json evidence = Json::object();
  bool pass = true;
  std::string detail = "provider fault handling verified";

  // Sub-case a: persistent 429 -> retries, then a clean terminal state.
  {
    MockProvider mock;
    if (auto status = mock.start(MockProvider::Mode::Always429); !status) {
      return make_result("H11", iteration, false, "mock provider failed to start");
    }
    ScenarioContext ctx(options, "H11", iteration);
    if (auto status = ctx.setup(mock_provider_config(mock.port())); !status) {
      return setup_failed("H11", iteration, status);
    }
    auto session = ctx.create_session();
    if (!session) {
      return make_result("H11", iteration, false, "session create failed");
    }
    send_prompt_with_model(ctx, *session, "Say hi.", "spike-mock", "mock-model");
    const bool saw_retry = ctx.events().wait_for(
        [](const agents::OpencodeEvent& event) {
          if (event.type == "session.status" &&
              event.properties.contains("status") &&
              event.properties["status"].is_object()) {
            return event.properties["status"].value("type", "") == "retry";
          }
          return event.type == "session.error";
        },
        30s);
    const bool idle = ctx.wait_session_idle(*session, 20s);
    if (!idle) {
      (void)ctx.client().interrupt(*session);
      ctx.wait_session_idle(*session, 10s);
    }
    evidence["always_429"] = Json{{"retry_or_error_observed", saw_retry},
                                  {"mock_requests", mock.requests()},
                                  {"final_text", ctx.last_assistant_text(*session)}};
    pass = pass && saw_retry;
    if (!saw_retry) {
      detail = "no retry/error surfaced for persistent 429";
    }
  }

  // Sub-case b: two 429s then success -> the turn completes with the mock text.
  {
    MockProvider mock;
    if (auto status = mock.start(MockProvider::Mode::FailTwiceThenSucceed); !status) {
      return make_result("H11", iteration, false, "mock provider failed to start (b)");
    }
    ScenarioContext ctx(options, "H11", iteration);
    if (auto status = ctx.setup(mock_provider_config(mock.port())); !status) {
      return setup_failed("H11", iteration, status);
    }
    auto session = ctx.create_session();
    if (!session) {
      return make_result("H11", iteration, false, "session create failed (b)");
    }
    send_prompt_with_model(ctx, *session, "Say hi.", "spike-mock", "mock-model");
    const bool idle = ctx.wait_session_idle(*session, budget(options));
    const std::string text = ctx.last_assistant_text(*session);
    evidence["fail_twice_then_succeed"] =
        Json{{"idle", idle},
             {"mock_requests", mock.requests()},
             {"final_text", text},
             {"contains_mock", text_contains(text, "MOCK-OK")}};
    pass = pass && idle && text_contains(text, "MOCK-OK");
    if (!text_contains(text, "MOCK-OK")) {
      detail = "provider recovery after 429 did not complete the turn";
    }
  }

  // Sub-case c: a hanging provider -> the session stays busy and a cancel ends it.
  {
    MockProvider mock;
    if (auto status = mock.start(MockProvider::Mode::Hang); !status) {
      return make_result("H11", iteration, false, "mock provider failed to start (c)");
    }
    ScenarioContext ctx(options, "H11", iteration);
    if (auto status = ctx.setup(mock_provider_config(mock.port())); !status) {
      return setup_failed("H11", iteration, status);
    }
    auto session = ctx.create_session();
    if (!session) {
      return make_result("H11", iteration, false, "session create failed (c)");
    }
    send_prompt_with_model(ctx, *session, "Say hi.", "spike-mock", "mock-model");
    std::this_thread::sleep_for(5s);
    const bool still_busy = !ctx.events().wait_for(
        [&](const agents::OpencodeEvent& event) {
          return event.type == "session.idle" &&
                 event.properties.value("sessionID", "") == *session;
        },
        500ms);
    (void)ctx.client().interrupt(*session);
    const bool idle_after_cancel = ctx.wait_session_idle(*session, 20s);
    evidence["hang"] = Json{{"still_busy_after_5s", still_busy},
                            {"idle_after_cancel", idle_after_cancel}};
    pass = pass && still_busy && idle_after_cancel;
    if (!idle_after_cancel) {
      detail = "hanging provider could not be cancelled cleanly";
    }
  }

  IterationResult result = make_result("H11", iteration, pass, detail);
  result.evidence = std::move(evidence);
  return result;
}

// H12: tool calling with each default model family.
IterationResult run_h12(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H12", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H12", iteration, status);
  }

  const std::vector<std::pair<std::string, std::string>> models = {
      {"deepseek", options.model_id},
      {"kimi", options.reviewer_model_id},
      {"glm", options.qa_model_id},
  };

  Json per_model = Json::object();
  bool pass = true;
  for (const auto& [family, model_id] : models) {
    auto session = ctx.create_session();
    if (!session) {
      per_model[family] = Json{{"ok", false}, {"error", "session create failed"}};
      pass = false;
      continue;
    }
    const std::string expected = "task-" + std::to_string(iteration) + "-" + family;
    send_prompt_with_model(ctx, 
        *session,
        "Call the cf_spike_echo tool exactly once with text=\"" + expected +
            "\". Then reply TOOL-DONE.",
        options.provider_id, model_id);
    const bool idle = ctx.wait_session_idle(*session, budget(options));
    bool called = false;
    for (const auto& call : ctx.mcp_calls()) {
      if (call.value("tool", "") == "spike_echo" &&
          call.value("arguments", Json::object()).value("text", "") == expected) {
        called = true;
      }
    }
    per_model[family] =
        Json{{"ok", idle && called},
             {"model", model_id},
             {"idle", idle},
             {"tool_called", called},
             {"tools", summarize_tools(ctx.tool_parts(*session))}};
    pass = pass && idle && called;
  }

  IterationResult result =
      make_result("H12", iteration, pass,
                  pass ? "all three model families completed the tool task"
                       : "one or more model families failed the tool task");
  result.evidence["per_model"] = per_model;
  return result;
}

// H13: permission request semantics (blocking, late replies, unknown IDs, restart).
IterationResult run_h13(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H13", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H13", iteration, status);
  }
  Json evidence = Json::object();
  bool pass = true;

  // Sub-case a: the session blocks while the request is pending, then continues.
  ctx.set_permission_policy([](const agents::OpencodeEvent&) { return "hold"; });
  auto session_a = ctx.create_session();
  if (!session_a) {
    return make_result("H13", iteration, false, "session create failed");
  }
  const std::string sid_a = *session_a;
  send_prompt(ctx, sid_a, "Run the bash command: echo h13-once. Then reply H13-A-DONE.");
  const bool asked = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" &&
               event.properties.value("sessionID", "") == sid_a;
      },
      60s);
  std::this_thread::sleep_for(3s);
  const bool idle_while_pending = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "session.idle" &&
               event.properties.value("sessionID", "") == sid_a;
      },
      500ms);
  std::string request_a;
  for (const auto& request : ctx.permission_requests()) {
    if (request.properties.value("sessionID", "") == sid_a) {
      request_a = request.properties.value("id", "");
    }
  }
  bool replied_late_ok = false;
  if (!request_a.empty()) {
    replied_late_ok = ctx.client().reply_permission(sid_a, request_a, "once").has_value();
  }
  const bool idle_a = ctx.wait_session_idle(sid_a, budget(options));
  evidence["blocking"] = Json{{"permission_asked", asked},
                              {"idle_while_pending", idle_while_pending},
                              {"reply_after_wait_ok", replied_late_ok},
                              {"idle_after_reply", idle_a}};
  pass = pass && asked && !idle_while_pending && idle_a;

  // Sub-case b: interrupt while a permission is pending; a late reply is not applied.
  auto session_b = ctx.create_session();
  if (!session_b) {
    return make_result("H13", iteration, false, "session create failed (b)");
  }
  const std::string sid_b = *session_b;
  send_prompt(ctx, sid_b, "Run the bash command: echo h13-late. Then reply H13-B-DONE.");
  const bool asked_b = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" &&
               event.properties.value("sessionID", "") == sid_b;
      },
      60s);
  std::string request_b;
  for (const auto& request : ctx.permission_requests()) {
    if (request.properties.value("sessionID", "") == sid_b) {
      request_b = request.properties.value("id", "");
    }
  }
  (void)ctx.client().interrupt(sid_b);
  const bool idle_b = ctx.wait_session_idle(sid_b, 30s);
  bool late_reply_ok = true;
  if (!request_b.empty()) {
    late_reply_ok = ctx.client().reply_permission(sid_b, request_b, "once").has_value();
  }
  const auto tools_b = ctx.tool_parts(sid_b);
  bool bash_completed_b = false;
  for (const auto& part : tools_b) {
    if (tool_name(part).find("bash") != std::string::npos &&
        tool_status(part) == "completed") {
      bash_completed_b = true;
    }
  }
  evidence["late_reply_after_cancel"] = Json{{"asked", asked_b},
                                             {"idle_after_interrupt", idle_b},
                                             {"late_reply_accepted", late_reply_ok},
                                             {"bash_completed", bash_completed_b}};
  pass = pass && asked_b && idle_b && !bash_completed_b;

  // Sub-case c: a reply to an unknown request ID fails cleanly.
  auto unknown = ctx.client().reply_permission(sid_a, "per_does_not_exist", "once");
  evidence["unknown_request_reply"] = Json{{"accepted", unknown.has_value()}};

  // Sub-case d: restart the server with a pending request; the request is gone.
  ctx.set_permission_policy([](const agents::OpencodeEvent&) { return "hold"; });
  auto session_d = ctx.create_session();
  if (!session_d) {
    return make_result("H13", iteration, false, "session create failed (d)");
  }
  const std::string sid_d = *session_d;
  send_prompt(ctx, sid_d, "Run the bash command: echo h13-restart. Then reply H13-D-DONE.");
  const bool asked_d = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" &&
               event.properties.value("sessionID", "") == sid_d;
      },
      60s);
  std::string request_d;
  for (const auto& request : ctx.permission_requests()) {
    if (request.properties.value("sessionID", "") == sid_d) {
      request_d = request.properties.value("id", "");
    }
  }
  if (auto status = ctx.restart_server(); !status) {
    return make_result("H13", iteration, false, "restart failed");
  }
  bool restart_reply_accepted = true;
  if (!request_d.empty()) {
    restart_reply_accepted =
        ctx.client().reply_permission(sid_d, request_d, "once").has_value();
  }
  (void)ctx.client().interrupt(sid_d);
  const bool idle_d = ctx.wait_session_idle(sid_d, 30s);
  evidence["restart_pending_request"] = Json{{"asked", asked_d},
                                             {"late_reply_accepted", restart_reply_accepted},
                                             {"idle_after_interrupt", idle_d}};
  pass = pass && asked_d && idle_d;

  IterationResult result =
      make_result("H13", iteration, pass,
                  pass ? "permission semantics behave as documented"
                       : "see evidence for sub-case outcomes");
  result.evidence = std::move(evidence);
  return result;
}

// H14: cancellation semantics for a running tool and a pending permission.
IterationResult run_h14(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H14", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H14", iteration, status);
  }
  Json evidence = Json::object();
  bool pass = true;

  // Sub-case a: cancel a running tool; no file change after the cancel.
  auto session_a = ctx.create_session();
  if (!session_a) {
    return make_result("H14", iteration, false, "session create failed");
  }
  const std::string sid_a = *session_a;
  send_prompt(ctx, sid_a,
             "Run the bash command: sleep 8 && echo LATE > h14.txt. Then reply "
             "H14-A-DONE.");
  const bool started = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" || event.type == "message.part.updated";
      },
      60s);
  std::this_thread::sleep_for(1500ms);
  (void)ctx.client().interrupt(sid_a);
  const bool idle_a = ctx.wait_session_idle(sid_a, 30s);
  std::this_thread::sleep_for(9s);
  const bool late_file = fs::exists(ctx.project_dir() / "h14.txt");
  evidence["running_tool"] = Json{{"started", started},
                                  {"idle_after_cancel", idle_a},
                                  {"late_file_created", late_file},
                                  {"tools", summarize_tools(ctx.tool_parts(sid_a))}};
  pass = pass && started && !late_file;

  // Sub-case b: cancel with a permission pending; no tool runs afterwards.
  ctx.set_permission_policy([](const agents::OpencodeEvent&) { return "hold"; });
  auto session_b = ctx.create_session();
  if (!session_b) {
    return make_result("H14", iteration, false, "session create failed (b)");
  }
  const std::string sid_b = *session_b;
  send_prompt(ctx, sid_b, "Run the bash command: echo h14-should-not-run. Then reply DONE.");
  const bool asked_b = ctx.events().wait_for(
      [&](const agents::OpencodeEvent& event) {
        return event.type == "permission.asked" &&
               event.properties.value("sessionID", "") == sid_b;
      },
      60s);
  (void)ctx.client().interrupt(sid_b);
  const bool idle_b = ctx.wait_session_idle(sid_b, 30s);
  const auto tools_b = ctx.tool_parts(sid_b);
  bool bash_completed_b = false;
  for (const auto& part : tools_b) {
    if (tool_name(part).find("bash") != std::string::npos &&
        tool_status(part) == "completed") {
      bash_completed_b = true;
    }
  }
  evidence["pending_permission"] = Json{{"asked", asked_b},
                                        {"idle_after_cancel", idle_b},
                                        {"bash_completed", bash_completed_b}};
  pass = pass && asked_b && idle_b && !bash_completed_b;

  IterationResult result =
      make_result("H14", iteration, pass,
                  pass ? "cancellation stops tools and pending permissions"
                       : "see evidence for sub-case outcomes");
  result.evidence = std::move(evidence);
  return result;
}

// H15: several sessions on one server, and session state after a restart.
IterationResult run_h15(const SpikeOptions& options, int iteration) {
  ScenarioContext ctx(options, "H15", iteration);
  if (auto status = ctx.setup(); !status) {
    return setup_failed("H15", iteration, status);
  }
  Json evidence = Json::object();
  bool pass = true;

  std::vector<std::string> sessions;
  for (int index = 1; index <= 3; ++index) {
    auto session = ctx.create_session();
    if (!session) {
      return make_result("H15", iteration, false, "session create failed");
    }
    sessions.push_back(*session);
    send_prompt(ctx, *session, "Reply with exactly S" + std::to_string(index) +
                             " and nothing else. Do not use any tools.");
  }
  bool all_idle = true;
  Json isolation = Json::object();
  for (std::size_t index = 0; index < sessions.size(); ++index) {
    const bool idle = ctx.wait_session_idle(sessions[index], budget(options));
    const std::string text = ctx.last_assistant_text(sessions[index]);
    const std::string expected = "S" + std::to_string(index + 1);
    all_idle = all_idle && idle;
    isolation["session_" + std::to_string(index + 1)] =
        Json{{"idle", idle}, {"text", text}, {"isolated", text_contains(text, expected)}};
    pass = pass && idle && text_contains(text, expected);
  }
  evidence["isolation"] = isolation;

  // Restart the server: sessions and history survive; a further turn works.
  if (auto status = ctx.restart_server(); !status) {
    return make_result("H15", iteration, false, "restart failed");
  }
  Json after_restart = Json::object();
  auto info = ctx.client().session_info(sessions[0]);
  after_restart["session_exists"] = info.has_value();
  const auto mark = ctx.events().mark();
  send_prompt(ctx, sessions[0],
              "Reply with exactly S1-AGAIN and nothing else. Do not use any tools.");
  const bool idle_again = ctx.wait_session_idle(sessions[0], budget(options), mark);
  const std::string text_again = ctx.last_assistant_text(sessions[0]);
  after_restart["idle_after_restart"] = idle_again;
  after_restart["text"] = text_again;
  after_restart["continued"] = text_contains(text_again, "S1-AGAIN");
  evidence["after_restart"] = after_restart;
  pass = pass && info.has_value() && idle_again && text_contains(text_again, "S1-AGAIN");

  IterationResult result =
      make_result("H15", iteration, pass,
                  pass ? "sessions isolated; state survived a server restart"
                       : "see evidence for sub-case outcomes");
  result.evidence = std::move(evidence);
  return result;
}

std::map<std::string, ScenarioFn> scenario_registry() {
  return {
      {"H1", run_h1},   {"H2", run_h2},   {"H3", run_h3},   {"H4", run_h4},
      {"H5", run_h5},   {"H6", run_h6},   {"H7", run_h7},   {"H8", run_h8},
      {"H9", run_h9},   {"H10", run_h10}, {"H11", run_h11}, {"H12", run_h12},
      {"H13", run_h13}, {"H14", run_h14}, {"H15", run_h15},
  };
}

}  // namespace cf::spike
