// cf-common: process execution for trusted commands (design section 6.7.3).
//
// Trusted steps are argument lists executed directly: no shell, no string
// concatenation. Used by the gate runner and the git layer; on a real run the
// sandbox executor (design section 6.8) implements the same interface.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace cf::process {

struct ProcessRequest {
  std::vector<std::string> argv;  // argv[0] is the program, resolved via PATH
  std::string working_directory;  // empty: inherit the parent's directory
  std::vector<std::pair<std::string, std::string>> environment;  // added vars
  std::chrono::milliseconds timeout{600000};
  std::size_t output_limit_bytes = 1u << 20;  // per stream
};

struct ProcessResult {
  bool launched = false;  // false when exec failed (program not found, ...)
  int exit_code = -1;
  bool timed_out = false;
  std::string error;  // launch failure detail
  std::string stdout_text;
  std::string stderr_text;
  bool truncated_stdout = false;
  bool truncated_stderr = false;
  int64_t duration_ms = 0;
};

class ProcessExecutor {
 public:
  virtual ~ProcessExecutor() = default;
  virtual ProcessResult run(const ProcessRequest& request) = 0;
};

class LocalProcessExecutor final : public ProcessExecutor {
 public:
  ProcessResult run(const ProcessRequest& request) override;
};

// Supervises an already-forked child: captures stdout/stderr (up to the
// request's limit), enforces the timeout by killing the child's process group,
// and reports exec/setup failures written to a CLOEXEC pipe. Used by
// LocalProcessExecutor and by sandbox executors that need their own fork and
// child setup (namespaces, Landlock, seccomp).
ProcessResult supervise_child(pid_t pid, int stdout_fd, int stderr_fd, int exec_fd,
                              const ProcessRequest& request);

}  // namespace cf::process
