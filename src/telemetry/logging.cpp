#include "cf/telemetry/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/formatter.h>
#include <spdlog/spdlog.h>

namespace cf::telemetry {
namespace {

using Json = nlohmann::json;

thread_local LogContext g_context;

std::string time_string(std::chrono::system_clock::time_point time) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[32] = {0};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

std::string to_std_string(spdlog::string_view_t text) {
  return std::string(text.data(), text.size());
}

// One JSON object per line (design section 14). The fields are always
// present, even when the calling thread has no run context, so log consumers
// can rely on the schema.
class JsonFormatter final : public spdlog::formatter {
 public:
  void format(const spdlog::details::log_msg& msg,
              spdlog::memory_buf_t& dest) override {
    const LogContext& context = g_context;
    Json event = {{"time", time_string(msg.time)},
                  {"level", to_std_string(spdlog::level::to_string_view(msg.level))},
                  {"logger", to_std_string(msg.logger_name)},
                  {"message", to_std_string(msg.payload)},
                  {"run_id", context.run_id},
                  {"milestone_id", context.milestone_id},
                  {"agent", context.agent},
                  {"trace_id", context.trace_id}};
    if (msg.source.filename != nullptr) {
      event["file"] = msg.source.filename;
      event["line"] = msg.source.line;
    }
    std::string line = event.dump();
    line.push_back('\n');
    dest.append(line.data(), line.data() + line.size());
  }

  std::unique_ptr<spdlog::formatter> clone() const override {
    return std::make_unique<JsonFormatter>();
  }
};

}  // namespace

LogContext current_context() { return g_context; }

void set_context(const LogContext& context) {
  g_context = context;
  if (g_context.trace_id.empty()) {
    g_context.trace_id = new_trace_id();
  }
}

ScopedContext::ScopedContext(LogContext context) : previous_(g_context) {
  set_context(context);
}

ScopedContext::ScopedContext(std::string run_id, std::string milestone_id,
                             std::string agent, std::string trace_id)
    : ScopedContext(LogContext{std::move(run_id), std::move(milestone_id),
                               std::move(agent), std::move(trace_id)}) {}

ScopedContext::~ScopedContext() { g_context = std::move(previous_); }

std::string new_trace_id() {
  static std::atomic<std::uint64_t> counter{0};
  std::random_device random;
  const std::uint64_t value =
      (static_cast<std::uint64_t>(random()) << 32) ^
      static_cast<std::uint64_t>(random()) ^
      counter.fetch_add(1, std::memory_order_relaxed);
  char buffer[17] = {0};
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

void install_json_formatter(const std::shared_ptr<spdlog::logger>& logger) {
  if (logger == nullptr) {
    return;
  }
  logger->set_formatter(std::make_unique<JsonFormatter>());
}

void configure_json_logging(spdlog::level::level_enum level) {
  spdlog::set_level(level);
  const auto install = [level](const std::shared_ptr<spdlog::logger>& logger) {
    if (logger == nullptr) {
      return;
    }
    install_json_formatter(logger);
    logger->set_level(level);
  };
  spdlog::apply_all(install);
  if (auto logger = spdlog::default_logger()) {
    install(logger);
  }
}

}  // namespace cf::telemetry
