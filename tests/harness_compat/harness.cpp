// Phase 0a harness compatibility spike: infrastructure (design section 6.6.2).
#include "harness.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace cf::spike {

namespace {

Json merge_json(Json base, const Json& overrides) {
  if (!overrides.is_object()) {
    return overrides;
  }
  for (auto it = overrides.begin(); it != overrides.end(); ++it) {
    if (it->is_object() && base.contains(it.key()) && base[it.key()].is_object()) {
      base[it.key()] = merge_json(base[it.key()], *it);
    } else {
      base[it.key()] = *it;
    }
  }
  return base;
}

}  // namespace

// --- EventLog ---------------------------------------------------------------

void EventLog::push(const agents::OpencodeEvent& event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
  }
  cv_.notify_all();
}

std::vector<agents::OpencodeEvent> EventLog::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_;
}

std::vector<agents::OpencodeEvent> EventLog::of_type(std::string_view type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<agents::OpencodeEvent> out;
  for (const auto& event : events_) {
    if (event.type == type) {
      out.push_back(event);
    }
  }
  return out;
}

std::optional<agents::OpencodeEvent> EventLog::first_of_type(std::string_view type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& event : events_) {
    if (event.type == type) {
      return event;
    }
  }
  return std::nullopt;
}

bool EventLog::wait_for(
    const std::function<bool(const agents::OpencodeEvent&)>& predicate,
    std::chrono::milliseconds timeout) {
  return wait_for_since(0, predicate, timeout);
}

std::size_t EventLog::mark() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.size();
}

bool EventLog::wait_for_since(
    std::size_t from,
    const std::function<bool(const agents::OpencodeEvent&)>& predicate,
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] {
    for (std::size_t index = from; index < events_.size(); ++index) {
      if (predicate(events_[index])) {
        return true;
      }
    }
    return false;
  });
}

Json EventLog::type_counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Json counts = Json::object();
  for (const auto& event : events_) {
    counts[event.type] = counts.value(event.type, 0) + 1;
  }
  return counts;
}

// --- OpencodeServer ---------------------------------------------------------

OpencodeServer::~OpencodeServer() { terminate(); }

uint16_t pick_free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const uint16_t port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

Status OpencodeServer::start(const Options& options) {
  port_ = options.port != 0 ? options.port : pick_free_port();
  if (port_ == 0) {
    return fail("SPIKE_NO_PORT", "could not pick a free port");
  }

  const std::string log_path = "server.log";  // opened after chdir into the project
  const pid_t pid = ::fork();
  if (pid < 0) {
    return fail("SPIKE_FORK_FAILED", std::strerror(errno));
  }
  if (pid == 0) {
    ::setsid();
    if (::chdir(options.project_dir.c_str()) != 0) {
      ::_exit(126);
    }
    for (const auto& [key, value] : options.env) {
      ::setenv(key.c_str(), value.c_str(), 1);
    }
    const int fd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd >= 0) {
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
    }
    const std::string port = std::to_string(port_);
    std::vector<std::string> args = {options.binary, "serve", "--port", port,
                                     "--hostname", "127.0.0.1", "--pure"};
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }

  pid_ = pid;
  return {};
}

bool OpencodeServer::healthy(std::chrono::milliseconds timeout,
                             agents::OpencodeClient& client) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto health = client.health();
    if (health && health->value("healthy", false)) {
      return true;
    }
    if (!running()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

bool OpencodeServer::running() const {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, WNOHANG);
  return result == 0;
}

void OpencodeServer::kill() {
  if (pid_ <= 0) {
    return;
  }
  ::kill(-pid_, SIGKILL);
  int status = 0;
  ::waitpid(pid_, &status, 0);
  pid_ = -1;
}

void OpencodeServer::terminate() {
  if (pid_ <= 0) {
    return;
  }
  ::kill(-pid_, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  kill();
}

// --- ScenarioContext --------------------------------------------------------

ScenarioContext::ScenarioContext(const SpikeOptions& options, std::string scenario,
                                 int iteration)
    : options_(options), scenario_(std::move(scenario)), iteration_(iteration) {
  std::error_code ec;
  const fs::path base = fs::absolute(options_.workdir, ec);
  const fs::path root = ec ? options_.workdir : base;
  run_dir_ = root / scenario_ / ("run-" + std::to_string(iteration_));
  project_dir_ = run_dir_ / "proj";
  calls_file_ = run_dir_ / "mcp-calls.jsonl";

  set_permission_policy([](const agents::OpencodeEvent&) { return "once"; });
  set_question_policy([](const agents::OpencodeEvent&) { return "reject"; });
}

ScenarioContext::~ScenarioContext() {
  if (stream_) {
    stream_->stop();
    stream_.reset();
  }
  client_.reset();
  if (!options_.keep_servers) {
    server_.terminate();
  }
}

Json ScenarioContext::config() const { return config_; }

void ScenarioContext::set_permission_policy(
    std::function<std::string(const agents::OpencodeEvent&)> policy) {
  std::lock_guard<std::mutex> lock(broker_mutex_);
  permission_policy_ = std::move(policy);
}

int ScenarioContext::permissions_answered() const {
  std::lock_guard<std::mutex> lock(broker_mutex_);
  return permissions_answered_;
}

std::vector<agents::OpencodeEvent> ScenarioContext::permission_requests() const {
  std::lock_guard<std::mutex> lock(broker_mutex_);
  return permission_requests_;
}

void ScenarioContext::set_question_policy(
    std::function<std::string(const agents::OpencodeEvent&)> policy) {
  std::lock_guard<std::mutex> lock(broker_mutex_);
  question_policy_ = std::move(policy);
}

std::vector<agents::OpencodeEvent> ScenarioContext::question_requests() const {
  std::lock_guard<std::mutex> lock(broker_mutex_);
  return question_requests_;
}

Status ScenarioContext::setup(const Json& config_overrides) {
  std::error_code ec;
  fs::create_directories(project_dir_, ec);
  if (ec) {
    return fail("SPIKE_MKDIR_FAILED", ec.message());
  }

  // A git repository keeps opencode's snapshot machinery happy.
  const std::string git_cmd = "git init -q '" + project_dir_.string() + "' >/dev/null 2>&1";
  if (std::system(git_cmd.c_str()) != 0) {
    return fail("SPIKE_GIT_INIT_FAILED", "git init failed in " + project_dir_.string());
  }

  return start_server_with(config_overrides);
}

Status ScenarioContext::start_server_with(const Json& config_overrides) {
  Json base = Json{
      {"$schema", "https://opencode.ai/config.json"},
      {"autoupdate", false},
      {"share", "disabled"},
      {"model", options_.provider_id + "/" + options_.model_id},
      {"small_model", options_.provider_id + "/" + options_.model_id},
      {"permission",
       Json{{"edit", "allow"},
            {"webfetch", "deny"},
            {"websearch", "deny"},
            {"question", "allow"},
            {"bash", Json{{"*", "ask"}}}}},
  };
  if (!options_.mcp_binary.empty()) {
    base["mcp"] = Json{{"cf",
                        Json{{"type", "local"},
                             {"command", Json::array({options_.mcp_binary})},
                             {"enabled", true},
                             {"timeout", 5000},
                             {"environment",
                              Json{{"CF_MCP_CALLS_FILE", calls_file_.string()},
                                   {"CF_MCP_NAME", "cf"}}}}}};
  }
  config_ = merge_json(base, config_overrides);

  {
    std::ofstream out(project_dir_ / "opencode.json");
    if (!out) {
      return fail("SPIKE_CONFIG_WRITE_FAILED",
                  "cannot write " + (project_dir_ / "opencode.json").string());
    }
    out << config_.dump(2) << "\n";
  }

  OpencodeServer::Options server_options;
  server_options.binary = options_.opencode_binary;
  server_options.project_dir = project_dir_;
  if (auto status = server_.start(server_options); !status) {
    return status;
  }

  agents::OpencodeClientOptions client_options;
  client_options.host = "127.0.0.1";
  client_options.port = server_.port();
  client_ = std::make_unique<agents::OpencodeClient>(client_options);
  if (!server_.healthy(std::chrono::seconds(30), *client_)) {
    return fail("SPIKE_SERVER_UNHEALTHY",
                "opencode serve did not become healthy on port " +
                    std::to_string(server_.port()));
  }

  open_stream();
  return {};
}

void ScenarioContext::open_stream() {
  stream_ = client_->open_event_stream("/event");
  stream_->set_callback([this](const agents::OpencodeEvent& event) { handle_event(event); });
  stream_->set_disconnect_callback([this](const std::string& reason) {
    agents::OpencodeEvent event;
    event.type = "spike.stream_disconnect";
    event.properties = Json{{"reason", reason}};
    events_.push(event);
  });
}

Status ScenarioContext::restart_server() {
  if (stream_) {
    stream_->stop();
    stream_.reset();
  }
  client_.reset();
  server_.terminate();

  OpencodeServer::Options server_options;
  server_options.binary = options_.opencode_binary;
  server_options.project_dir = project_dir_;
  if (auto status = server_.start(server_options); !status) {
    return status;
  }
  agents::OpencodeClientOptions client_options;
  client_options.host = "127.0.0.1";
  client_options.port = server_.port();
  client_ = std::make_unique<agents::OpencodeClient>(client_options);
  if (!server_.healthy(std::chrono::seconds(30), *client_)) {
    return fail("SPIKE_SERVER_UNHEALTHY", "opencode serve did not restart");
  }
  open_stream();
  return {};
}

void ScenarioContext::drop_stream() {
  if (stream_) {
    stream_->stop();
    stream_.reset();
  }
}

Status ScenarioContext::reopen_stream() {
  open_stream();
  return {};
}

void ScenarioContext::handle_event(const agents::OpencodeEvent& event) {
  events_.push(event);

  if (event.type == "permission.asked") {
    const std::string request_id = event.properties.value("id", "");
    const std::string session_id = event.properties.value("sessionID", "");
    std::function<std::string(const agents::OpencodeEvent&)> policy;
    {
      std::lock_guard<std::mutex> lock(broker_mutex_);
      permission_requests_.push_back(event);
      policy = permission_policy_;
    }
    if (policy && !request_id.empty()) {
      const std::string decision = policy(event);
      if (decision == "once" || decision == "reject") {
        auto reply = client_->reply_permission(session_id, request_id, decision);
        if (!reply) {
          spdlog::warn("permission reply failed for {}: {}", request_id,
                       reply.error().message);
        } else {
          std::lock_guard<std::mutex> lock(broker_mutex_);
          ++permissions_answered_;
        }
      }
    }
  } else if (event.type == "question.asked") {
    const std::string request_id = event.properties.value("id", "");
    const std::string session_id = event.properties.value("sessionID", "");
    std::function<std::string(const agents::OpencodeEvent&)> policy;
    {
      std::lock_guard<std::mutex> lock(broker_mutex_);
      question_requests_.push_back(event);
      policy = question_policy_;
    }
    const std::string decision = policy ? policy(event) : "reject";
    if (decision == "reject") {
      (void)client_->reject_question(session_id, request_id);
    } else if (decision == "answer_first") {
      std::vector<std::vector<std::string>> answers;
      if (event.properties.contains("questions")) {
        for (const auto& question : event.properties["questions"]) {
          std::vector<std::string> labels;
          if (question.contains("options") && !question["options"].empty()) {
            labels.push_back(question["options"][0].value("label", "yes"));
          } else {
            // A custom question with no options still needs an answer.
            labels.push_back("yes");
          }
          answers.push_back(labels);
        }
      }
      auto reply = client_->reply_question(session_id, request_id, answers);
      if (!reply) {
        spdlog::warn("question reply failed for {}: {}", request_id,
                     reply.error().message);
      }
    }
  }
}

Result<std::string> ScenarioContext::create_session() {
  auto response = client_->create_session(Json::object());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (!response->contains("id")) {
    return fail("SPIKE_NO_SESSION_ID", "session response has no id: " + response->dump());
  }
  return response->value("id", "");
}

Status ScenarioContext::prompt(const std::string& session_id, const std::string& text) {
  Json model = Json{{"providerID", options_.provider_id}, {"modelID", options_.model_id}};
  auto result = client_->prompt(session_id, text, model);
  if (!result) {
    return tl::make_unexpected(result.error());
  }
  return {};
}

Status ScenarioContext::prompt_with_model(const std::string& session_id,
                                          const std::string& text,
                                          const std::string& provider_id,
                                          const std::string& model_id) {
  Json model = Json{{"providerID", provider_id}, {"modelID", model_id}};
  auto result = client_->prompt(session_id, text, model);
  if (!result) {
    return tl::make_unexpected(result.error());
  }
  return {};
}

bool ScenarioContext::wait_session_idle(const std::string& session_id,
                                        std::chrono::milliseconds timeout,
                                        std::size_t since) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  // Phase 1: wait for the turn to show activity after the mark (a busy/retry
  // status or a message update). This avoids accepting a stale idle state
  // before opencode has started the new turn.
  const auto activity_deadline = std::min(
      deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5));
  while (std::chrono::steady_clock::now() < activity_deadline) {
    if (events_.wait_for_since(
            since,
            [&](const agents::OpencodeEvent& event) {
              if (event.properties.value("sessionID", "") != session_id) {
                return false;
              }
              if (event.type == "message.updated") {
                return true;
              }
              if (event.type == "session.status" && event.properties.contains("status") &&
                  event.properties["status"].is_object()) {
                const std::string type = event.properties["status"].value("type", "");
                return type == "busy" || type == "retry";
              }
              return false;
            },
            std::chrono::milliseconds(500))) {
      break;
    }
  }

  // Phase 2: wait for idle, ignoring events from before the mark.
  while (std::chrono::steady_clock::now() < deadline) {
    if (events_.wait_for_since(
            since,
            [&](const agents::OpencodeEvent& event) {
              return event.type == "session.idle" &&
                     event.properties.value("sessionID", "") == session_id;
            },
            std::chrono::milliseconds(500))) {
      return true;
    }
    auto status = client_->wait_idle(session_id, std::chrono::milliseconds(500));
    if (status.has_value()) {
      return true;
    }
  }
  return false;
}

bool ScenarioContext::wait_idle(const std::string& session_id,
                                std::chrono::milliseconds timeout) {
  return wait_session_idle(session_id, timeout);
}

std::string ScenarioContext::last_assistant_text(const std::string& session_id) const {
  auto messages = client_->session_messages(session_id);
  if (!messages || !messages->is_array()) {
    return "";
  }
  std::string text;
  for (auto it = messages->rbegin(); it != messages->rend(); ++it) {
    const auto& item = *it;
    if (!item.contains("info") || item["info"].value("role", "") != "assistant") {
      continue;
    }
    if (!item.contains("parts")) {
      continue;
    }
    for (const auto& part : item["parts"]) {
      if (part.value("type", "") == "text") {
        text += part.value("text", "");
      }
    }
    break;
  }
  return text;
}

std::vector<Json> ScenarioContext::tool_parts(const std::string& session_id) const {
  std::vector<Json> tools;
  auto messages = client_->session_messages(session_id);
  if (!messages || !messages->is_array()) {
    return tools;
  }
  for (const auto& item : *messages) {
    if (!item.contains("parts")) {
      continue;
    }
    for (const auto& part : item["parts"]) {
      if (part.value("type", "") == "tool") {
        tools.push_back(part);
      }
    }
  }
  return tools;
}

std::vector<Json> ScenarioContext::mcp_calls() const {
  std::vector<Json> calls;
  std::ifstream in(calls_file_);
  std::string line;
  while (std::getline(in, line)) {
    Json parsed = Json::parse(line, nullptr, false);
    if (!parsed.is_discarded()) {
      calls.push_back(parsed);
    }
  }
  return calls;
}

}  // namespace cf::spike
