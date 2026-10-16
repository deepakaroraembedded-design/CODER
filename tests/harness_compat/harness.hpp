// Phase 0a harness compatibility spike (design section 6.6.2).
//
// Drives one pinned opencode server per iteration through the C++ adapter and
// exercises scenarios H1-H15. Every scenario returns structured evidence so
// the go / conditional-go / no-go decision is auditable.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/agents/opencode_client.hpp"
#include "cf/common/result.hpp"

namespace cf::spike {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct SpikeOptions {
  std::string opencode_binary = "opencode";
  std::string mcp_binary;  // absolute path to cf-mcp
  std::string model_id = "accounts/fireworks/models/deepseek-v4p1-flash";
  std::string provider_id = "fireworks-ai";
  std::string reviewer_model_id = "accounts/fireworks/models/kimi-k3";
  std::string qa_model_id = "accounts/fireworks/models/glm-5p3";
  fs::path workdir = "/tmp/cf-spike";
  int runs = 1;             // iterations per scenario
  int timeout_s = 240;      // per-iteration budget
  bool keep_servers = false;
  std::vector<std::string> scenarios;
};

struct IterationResult {
  std::string scenario;
  int iteration = 0;
  bool pass = false;
  std::string detail;
  Json evidence = Json::object();
};

// Thread-safe event log with waiting helpers.
class EventLog {
 public:
  void push(const agents::OpencodeEvent& event);
  std::vector<agents::OpencodeEvent> snapshot() const;
  std::vector<agents::OpencodeEvent> of_type(std::string_view type) const;
  std::optional<agents::OpencodeEvent> first_of_type(std::string_view type) const;
  bool wait_for(const std::function<bool(const agents::OpencodeEvent&)>& predicate,
                std::chrono::milliseconds timeout);
  // Number of events seen so far; used to ignore events from earlier turns.
  std::size_t mark() const;
  bool wait_for_since(std::size_t from,
                      const std::function<bool(const agents::OpencodeEvent&)>& predicate,
                      std::chrono::milliseconds timeout);
  Json type_counts() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<agents::OpencodeEvent> events_;
};

// opencode serve child process.
class OpencodeServer {
 public:
  struct Options {
    std::string binary = "opencode";
    fs::path project_dir;
    uint16_t port = 0;
    std::vector<std::pair<std::string, std::string>> env;
  };

  OpencodeServer() = default;
  ~OpencodeServer();
  OpencodeServer(const OpencodeServer&) = delete;
  OpencodeServer& operator=(const OpencodeServer&) = delete;

  Status start(const Options& options);
  bool healthy(std::chrono::milliseconds timeout, agents::OpencodeClient& client);
  void kill();       // SIGKILL the whole process group
  void terminate();  // SIGTERM, wait up to 5 s, then SIGKILL
  bool running() const;
  pid_t pid() const { return pid_; }
  uint16_t port() const { return port_; }

 private:
  pid_t pid_ = -1;
  uint16_t port_ = 0;
};

uint16_t pick_free_port();

// Per-iteration context: fresh project directory, server, adapter, event
// stream and permission/question brokers.
class ScenarioContext {
 public:
  ScenarioContext(const SpikeOptions& options, std::string scenario, int iteration);
  ~ScenarioContext();
  ScenarioContext(const ScenarioContext&) = delete;
  ScenarioContext& operator=(const ScenarioContext&) = delete;

  // Writes the project config (merged over the spike defaults), starts the
  // server and opens the event stream.
  Status setup(const Json& config_overrides = Json::object());
  // Restarts the opencode server (scenarios H13/H15); the event stream is
  // re-opened automatically.
  Status restart_server();
  // Drops the SSE stream without stopping the server, then re-opens it
  // (scenario H7).
  void drop_stream();
  Status reopen_stream();

  agents::OpencodeClient& client() { return *client_; }
  OpencodeServer& server() { return server_; }
  EventLog& events() { return events_; }
  const SpikeOptions& options() const { return options_; }
  fs::path project_dir() const { return project_dir_; }
  fs::path run_dir() const { return run_dir_; }
  fs::path calls_file() const { return calls_file_; }

  // --- session helpers ----------------------------------------------------
  Result<std::string> create_session();
  Status prompt(const std::string& session_id, const std::string& text);
  Status prompt_with_model(const std::string& session_id, const std::string& text,
                           const std::string& provider_id,
                           const std::string& model_id);
  bool wait_idle(const std::string& session_id, std::chrono::milliseconds timeout);
  // Waits for a turn to become idle. `since` is an EventLog mark taken before
  // the prompt; events before it (for example an earlier turn's idle event)
  // are ignored.
  bool wait_session_idle(const std::string& session_id,
                         std::chrono::milliseconds timeout, std::size_t since = 0);
  std::string last_assistant_text(const std::string& session_id) const;
  std::vector<Json> tool_parts(const std::string& session_id) const;
  std::vector<Json> mcp_calls() const;

  // --- brokers ------------------------------------------------------------
  // Permission policy: given the request event, return "once", "reject" or
  // "hold" (the scenario answers manually). Default: "once".
  void set_permission_policy(std::function<std::string(const agents::OpencodeEvent&)> policy);
  int permissions_answered() const;
  std::vector<agents::OpencodeEvent> permission_requests() const;

  // Question policy: "answer_first", "reject" or "hold". Default: "reject".
  void set_question_policy(std::function<std::string(const agents::OpencodeEvent&)> policy);
  std::vector<agents::OpencodeEvent> question_requests() const;

  // --- misc ---------------------------------------------------------------
  const std::string& scenario() const { return scenario_; }
  int iteration() const { return iteration_; }
  Json config() const;

 private:
  void handle_event(const agents::OpencodeEvent& event);
  void open_stream();
  Status start_server_with(const Json& config);

  SpikeOptions options_;
  std::string scenario_;
  int iteration_ = 0;
  fs::path run_dir_;
  fs::path project_dir_;
  fs::path calls_file_;
  OpencodeServer server_;
  std::unique_ptr<agents::OpencodeClient> client_;
  std::unique_ptr<agents::EventStream> stream_;
  EventLog events_;
  Json config_ = Json::object();

  mutable std::mutex broker_mutex_;
  std::function<std::string(const agents::OpencodeEvent&)> permission_policy_;
  std::function<std::string(const agents::OpencodeEvent&)> question_policy_;
  int permissions_answered_ = 0;
  std::vector<agents::OpencodeEvent> permission_requests_;
  std::vector<agents::OpencodeEvent> question_requests_;
};

using ScenarioFn = std::function<IterationResult(const SpikeOptions&, int iteration)>;

IterationResult run_h1(const SpikeOptions&, int);
IterationResult run_h2(const SpikeOptions&, int);
IterationResult run_h3(const SpikeOptions&, int);
IterationResult run_h4(const SpikeOptions&, int);
IterationResult run_h5(const SpikeOptions&, int);
IterationResult run_h6(const SpikeOptions&, int);
IterationResult run_h7(const SpikeOptions&, int);
IterationResult run_h8(const SpikeOptions&, int);
IterationResult run_h9(const SpikeOptions&, int);
IterationResult run_h10(const SpikeOptions&, int);
IterationResult run_h11(const SpikeOptions&, int);
IterationResult run_h12(const SpikeOptions&, int);
IterationResult run_h13(const SpikeOptions&, int);
IterationResult run_h14(const SpikeOptions&, int);
IterationResult run_h15(const SpikeOptions&, int);

std::map<std::string, ScenarioFn> scenario_registry();

}  // namespace cf::spike
