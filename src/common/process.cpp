#include "cf/gates/process.hpp"

#include <cerrno>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cf::process {
namespace {

class Pipe {
 public:
  Pipe() {
    if (::pipe(fds_) != 0) {
      fds_[0] = fds_[1] = -1;
    }
  }
  ~Pipe() {
    close_read();
    close_write();
  }
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  int read_fd() const { return fds_[0]; }
  int write_fd() const { return fds_[1]; }
  bool valid() const { return fds_[0] >= 0 && fds_[1] >= 0; }

  void close_read() {
    if (fds_[0] >= 0) {
      ::close(fds_[0]);
      fds_[0] = -1;
    }
  }
  void close_write() {
    if (fds_[1] >= 0) {
      ::close(fds_[1]);
      fds_[1] = -1;
    }
  }

  // Hands the read end to the caller (supervise_child closes it).
  int release_read() {
    const int fd = fds_[0];
    fds_[0] = -1;
    return fd;
  }

 private:
  int fds_[2] = {-1, -1};
};

void append_limited(std::string& target, const char* data, std::size_t size,
                    std::size_t limit, bool& truncated) {
  const std::size_t room = target.size() < limit ? limit - target.size() : 0;
  const std::size_t take = size < room ? size : room;
  target.append(data, take);
  if (take < size) {
    truncated = true;
  }
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

}  // namespace

ProcessResult supervise_child(pid_t pid, int stdout_fd, int stderr_fd,
                              int exec_fd, const ProcessRequest& request) {
  ProcessResult result;
  const auto start = std::chrono::steady_clock::now();
  const auto finish = [&result, start]() {
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  };

  const auto deadline = start + request.timeout;
  bool stdout_open = stdout_fd >= 0;
  bool stderr_open = stderr_fd >= 0;
  char buffer[8192];

  while (stdout_open || stderr_open) {
    struct pollfd fds[2];
    int count = 0;
    if (stdout_open) {
      fds[count].fd = stdout_fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    if (stderr_open) {
      fds[count].fd = stderr_fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    const int wait_ms = remaining_ms(deadline);
    if (wait_ms <= 0) {
      result.timed_out = true;
      break;
    }
    const int ready = ::poll(fds, static_cast<nfds_t>(count), wait_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = std::string("poll: ") + std::strerror(errno);
      break;
    }
    if (ready == 0) {
      result.timed_out = true;
      break;
    }
    for (int i = 0; i < count; ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      const bool is_stdout = fds[i].fd == stdout_fd;
      const ssize_t got = ::read(fds[i].fd, buffer, sizeof(buffer));
      if (got > 0) {
        if (is_stdout) {
          append_limited(result.stdout_text, buffer, static_cast<std::size_t>(got),
                         request.output_limit_bytes, result.truncated_stdout);
        } else {
          append_limited(result.stderr_text, buffer, static_cast<std::size_t>(got),
                         request.output_limit_bytes, result.truncated_stderr);
        }
      } else if (got == 0) {
        if (is_stdout) {
          stdout_open = false;
        } else {
          stderr_open = false;
        }
      } else if (errno != EINTR && errno != EAGAIN) {
        if (is_stdout) {
          stdout_open = false;
        } else {
          stderr_open = false;
        }
      }
    }
  }
  if (stdout_fd >= 0) {
    ::close(stdout_fd);
  }
  if (stderr_fd >= 0) {
    ::close(stderr_fd);
  }

  if (result.timed_out) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
  }

  int status = 0;
  bool reaped = false;
  while (true) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      reaped = true;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!reaped) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }

  int exec_errno = 0;
  if (exec_fd >= 0) {
    const ssize_t exec_read = ::read(exec_fd, &exec_errno, sizeof(exec_errno));
    ::close(exec_fd);
    if (exec_read == static_cast<ssize_t>(sizeof(exec_errno)) && exec_errno != 0) {
      result.launched = false;
      result.error = std::string("exec: ") + std::strerror(exec_errno);
      finish();
      return result;
    }
  }
  result.launched = true;

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  } else {
    result.exit_code = -1;
  }
  finish();
  return result;
}

ProcessResult LocalProcessExecutor::run(const ProcessRequest& request) {
  if (request.argv.empty() || request.argv[0].empty()) {
    ProcessResult result;
    result.error = "empty argv";
    return result;
  }

  Pipe out_pipe;
  Pipe err_pipe;
  Pipe exec_pipe;  // close-on-exec: reports an exec failure to the parent
  if (!out_pipe.valid() || !err_pipe.valid() || !exec_pipe.valid()) {
    ProcessResult result;
    result.error = std::string("pipe: ") + std::strerror(errno);
    return result;
  }
  ::fcntl(exec_pipe.write_fd(), F_SETFD, FD_CLOEXEC);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ProcessResult result;
    result.error = std::string("fork: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    // Child: its own process group so the whole tree can be killed on timeout.
    ::setpgid(0, 0);
    ::dup2(out_pipe.write_fd(), STDOUT_FILENO);
    ::dup2(err_pipe.write_fd(), STDERR_FILENO);
    out_pipe.close_read();
    out_pipe.close_write();
    err_pipe.close_read();
    err_pipe.close_write();
    exec_pipe.close_read();
    if (!request.working_directory.empty() &&
        ::chdir(request.working_directory.c_str()) != 0) {
      const int err = errno;
      const auto written = ::write(exec_pipe.write_fd(), &err, sizeof(err));
      (void)written;
      ::_exit(127);
    }
    for (const auto& [key, value] : request.environment) {
      ::setenv(key.c_str(), value.c_str(), 1);
    }
    std::vector<char*> argv;
    argv.reserve(request.argv.size() + 1);
    for (const auto& argument : request.argv) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    const int err = errno;
    const auto written = ::write(exec_pipe.write_fd(), &err, sizeof(err));
    (void)written;
    ::_exit(127);
  }

  out_pipe.close_write();
  err_pipe.close_write();
  exec_pipe.close_write();
  return supervise_child(pid, out_pipe.release_read(), err_pipe.release_read(),
                         exec_pipe.release_read(), request);
}

}  // namespace cf::process
