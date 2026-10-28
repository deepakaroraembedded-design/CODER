// cf-telemetry: structured logging (design sections 6.11, 14).
//
// Every log line is one JSON object carrying run_id, milestone_id, agent and
// trace_id, so a run can be reconstructed from the logs. The context is
// thread-local: the run process sets the run, and each agent turn or gate run
// opens a ScopedContext with its milestone, role and a fresh trace id.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace cf::telemetry {

struct LogContext {
  std::string run_id;
  std::string milestone_id;
  std::string agent;
  std::string trace_id;
};

// The context of the calling thread.
LogContext current_context();

// Writes the structured context for the calling thread. Prefer ScopedContext.
void set_context(const LogContext& context);

// Restores the previous thread context on destruction.
class ScopedContext {
 public:
  ScopedContext(std::string run_id, std::string milestone_id, std::string agent,
                std::string trace_id = {});
  explicit ScopedContext(LogContext context);
  ~ScopedContext();

  ScopedContext(const ScopedContext&) = delete;
  ScopedContext& operator=(const ScopedContext&) = delete;

 private:
  LogContext previous_;
};

// A fresh 16-hex-character trace id.
std::string new_trace_id();

// Installs the JSON formatter on one logger without touching its level.
void install_json_formatter(const std::shared_ptr<spdlog::logger>& logger);

// Makes the default logger and every registered logger emit one JSON object
// per line (design section 14). Safe to call more than once. The caller picks
// the sinks; this only installs the formatter and level.
void configure_json_logging(spdlog::level::level_enum level = spdlog::level::info);

}  // namespace cf::telemetry
