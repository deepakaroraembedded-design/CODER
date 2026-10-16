#include "cf/sandbox/namespaces.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cf::sandbox {
namespace {

class Pipe {
 public:
  Pipe() {
    if (::pipe(fds_) != 0) {
      fds_[0] = -1;
      fds_[1] = -1;
    }
  }
  ~Pipe() {
    close_read();
    close_write();
  }
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  bool valid() const { return fds_[0] >= 0 && fds_[1] >= 0; }
  int read_fd() const { return fds_[0]; }
  int write_fd() const { return fds_[1]; }

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

[[noreturn]] void fail(int exec_fd, int error) {
  const auto written = ::write(exec_fd, &error, sizeof(error));
  (void)written;
  ::_exit(127);
}

// The sandbox root holds the (empty) mount point directories on the host.
void remove_root(const std::string& root) {
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

bool write_file(const std::string& path, const std::string& content) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    return false;
  }
  const auto written = ::write(fd, content.data(), content.size());
  ::close(fd);
  return written == static_cast<ssize_t>(content.size());
}

bool exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

int make_dir(const std::string& path, mode_t mode) {
  if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

// mkdir -p for the host-side worktree paths.
int make_dirs(const std::string& path, mode_t mode) {
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : -1;
  }
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    if (make_dirs(path.substr(0, slash), mode) != 0) {
      return -1;
    }
  }
  if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

int bind_mount(const std::string& source, const std::string& target, bool readonly) {
  if (::mount(source.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
    return -1;
  }
  if (readonly && ::mount(nullptr, target.c_str(), nullptr,
                          MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
    return -1;
  }
  return 0;
}

std::string host_path_to_container(const NamespaceSandboxSpec& spec, const std::string& path) {
  if (spec.worktree.empty()) {
    return spec.container_workdir;
  }
  if (path == spec.worktree) {
    return spec.container_workdir;
  }
  const std::string prefix = spec.worktree + "/";
  if (path.rfind(prefix, 0) == 0) {
    return spec.container_workdir + "/" + path.substr(prefix.size());
  }
  return spec.container_workdir;
}

// Builds the sandbox root and pivots into it. Runs as root inside the new
// user namespace; returns 0 or -1 with errno set. The root path is chosen by
// the parent (inside the pid namespace getpid() would always be 1).
int setup_root(const NamespaceSandboxSpec& spec, const std::string& root) {
  if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
    return -1;
  }
  if (make_dir(root, 0755) != 0) {
    return -1;
  }
  for (const char* dir : {"/work", "/build", "/tmp", "/home", "/home/cf", "/dev", "/proc",
                          "/usr", "/usr/local", "/bin", "/sbin", "/lib", "/lib64",
                          "/etc"}) {
    if (make_dir(root + dir, 0755) != 0) {
      return -1;
    }
  }

  // Host toolchain, read-only. Only the system directories are bound: /home,
  // /root, /var, /opt and /srv stay invisible (design section 6.8.1).
  for (const char* dir : {"/usr", "/usr/local", "/bin", "/sbin", "/lib", "/lib64", "/etc"}) {
    if (exists(dir) && bind_mount(dir, root + dir, true) != 0) {
      return -1;
    }
  }

  if (!spec.worktree.empty()) {
    // Missing writable directories are created in the host worktree first
    // (docker's bind mounts do the same); empty directories are invisible to
    // git, so this does not affect the recorded diff.
    for (const auto& path : spec.writable_paths) {
      if (make_dirs(spec.worktree + "/" + path, 0755) != 0) {
        return -1;
      }
    }
    if (bind_mount(spec.worktree, root + "/work", true) != 0) {
      return -1;
    }
    for (const auto& path : spec.writable_paths) {
      const std::string source = spec.worktree + "/" + path;
      const std::string target = root + "/work/" + path;
      if (make_dir(target, 0755) != 0) {
        return -1;
      }
      if (bind_mount(source, target, false) != 0) {
        return -1;
      }
    }
  }
  for (const auto& [source, target] : spec.extra_readonly_mounts) {
    if (make_dir(root + target, 0755) != 0) {
      return -1;
    }
    if (bind_mount(source, root + target, true) != 0) {
      return -1;
    }
  }

  // Fresh tmpfs for /tmp, $HOME (noexec) and the executable /build.
  const std::string tmp_options = "size=" + std::to_string(spec.tmp_mb) + "m,mode=1777";
  if (::mount("tmpfs", (root + "/tmp").c_str(), "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              tmp_options.c_str()) != 0) {
    return -1;
  }
  const std::string home_options = "size=" + std::to_string(spec.tmp_mb) + "m";
  if (::mount("tmpfs", (root + "/home/cf").c_str(), "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              home_options.c_str()) != 0) {
    return -1;
  }
  const std::string build_options = "size=" + std::to_string(spec.build_mb) + "m";
  if (::mount("tmpfs", (root + "/build").c_str(), "tmpfs", MS_NOSUID | MS_NODEV,
              build_options.c_str()) != 0) {
    return -1;
  }

  // Device nodes common tools expect (a user namespace cannot mount devtmpfs).
  if (::mount("tmpfs", (root + "/dev").c_str(), "tmpfs", MS_NOSUID, "mode=0755") != 0) {
    return -1;
  }
  for (const char* device : {"/dev/null", "/dev/zero", "/dev/full", "/dev/random",
                             "/dev/urandom"}) {
    const std::string target = root + device;
    const int fd = ::open(target.c_str(), O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
      return -1;
    }
    ::close(fd);
    if (exists(device) && bind_mount(device, target, false) != 0) {
      return -1;
    }
  }

  // A private procfs: the command is PID 1 in its own pid namespace, so this
  // shows only sandbox processes (design section 6.8.2).
  if (::mount("proc", (root + "/proc").c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              nullptr) != 0) {
    return -1;
  }

  // pivot_root needs the new root to be a mount point of its own.
  if (::mount(root.c_str(), root.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
    return -1;
  }
  if (::chdir(root.c_str()) != 0) {
    return -1;
  }
  if (::syscall(SYS_pivot_root, ".", ".") != 0) {
    return -1;
  }
  if (::umount2(".", MNT_DETACH) != 0) {
    return -1;
  }
  if (::chdir("/") != 0) {
    return -1;
  }
  return 0;
}

#if defined(__x86_64__)
constexpr unsigned long kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr unsigned long kAuditArch = AUDIT_ARCH_AARCH64;
#else
#error "unsupported architecture for the namespace sandbox"
#endif

constexpr __u64 kLandlockRead =
    LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

// Confines file access to the sandbox layout. Rules attach to each mounted
// subtree (bind mounts keep their original object hierarchy, so one rule on
// "/" would not cover them); Landlock ORs the rules, so the writable paths
// layered inside the read-only worktree stay writable.
int apply_landlock(const NamespaceSandboxSpec& spec) {
  const int abi = static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
  if (abi < 1) {
    return -1;
  }
  __u64 handled = kLandlockRead | LANDLOCK_ACCESS_FS_WRITE_FILE |
                  LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                  LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                  LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                  LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
  if (abi >= 2) {
    handled |= LANDLOCK_ACCESS_FS_REFER;
  }
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
  if (abi >= 3) {
    handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
  }
#endif
  struct landlock_ruleset_attr ruleset_attr {};
  ruleset_attr.handled_access_fs = handled;
  const int ruleset_fd = static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, &ruleset_attr, sizeof(ruleset_attr), 0));
  if (ruleset_fd < 0) {
    return -1;
  }

  struct Rule {
    std::string path;
    __u64 access;
  };
  std::vector<Rule> rules;
  for (const char* path : {"/", "/usr", "/usr/local", "/bin", "/sbin", "/lib", "/lib64",
                           "/etc", "/proc", "/work"}) {
    if (!spec.worktree.empty() || std::strcmp(path, "/work") != 0) {
      rules.push_back(Rule{path, kLandlockRead});
    }
  }
  for (const char* path : {"/dev", "/build", "/tmp", "/home/cf"}) {
    rules.push_back(Rule{path, handled});
  }
  for (const auto& path : spec.writable_paths) {
    rules.push_back(Rule{spec.container_workdir + "/" + path, handled});
  }
  for (const auto& [source, target] : spec.extra_readonly_mounts) {
    (void)source;
    rules.push_back(Rule{target, kLandlockRead});
  }

  for (const auto& rule : rules) {
    const int path_fd = ::open(rule.path.c_str(), O_PATH | O_CLOEXEC);
    if (path_fd < 0) {
      if (errno == ENOENT) {
        continue;
      }
      ::close(ruleset_fd);
      return -1;
    }
    struct landlock_path_beneath_attr attr {};
    attr.allowed_access = rule.access;
    attr.parent_fd = path_fd;
    const int rc = static_cast<int>(
        ::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0));
    ::close(path_fd);
    if (rc != 0) {
      ::close(ruleset_fd);
      return -1;
    }
  }

  if (::syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0) {
    ::close(ruleset_fd);
    return -1;
  }
  ::close(ruleset_fd);
  return 0;
}

// Removes the kernel attack surface the sandbox does not need. Denied calls
// fail with EPERM (design section 6.8.2); the arch check kills foreign-ABI
// callers.
int apply_seccomp() {
  constexpr unsigned kDeniedErrno = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
  const int denied[] = {
      SYS_ptrace,          SYS_process_vm_readv, SYS_process_vm_writev, SYS_bpf,
      SYS_userfaultfd,     SYS_perf_event_open,  SYS_keyctl,             SYS_add_key,
      SYS_request_key,     SYS_io_uring_setup,   SYS_io_uring_enter,     SYS_io_uring_register,
      SYS_mount,           SYS_umount2,          SYS_pivot_root,         SYS_chroot,
      SYS_unshare,         SYS_setns,            SYS_open_by_handle_at,  SYS_kcmp,
      SYS_pidfd_getfd,     SYS_kexec_load,       SYS_kexec_file_load,    SYS_init_module,
      SYS_finit_module,    SYS_delete_module,    SYS_reboot,             SYS_swapon,
      SYS_swapoff,         SYS_acct,             SYS_settimeofday,       SYS_clock_settime,
      SYS_adjtimex,        SYS_sethostname,      SYS_setdomainname,
  };
  std::vector<sock_filter> filter;
  filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
  filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0));
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));
  for (const int nr : denied) {
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(nr), 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, kDeniedErrno));
  }
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

  struct sock_fprog program {};
  program.len = static_cast<unsigned short>(filter.size());
  program.filter = filter.data();
  if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
    return -1;
  }
  return 0;
}

std::string current_cgroup_dir() {
  std::ifstream in("/proc/self/cgroup");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("0::", 0) == 0) {
      return "/sys/fs/cgroup" + line.substr(3);
    }
  }
  return {};
}

// cgroup v2 limits under the delegated subtree; inactive when delegation is
// missing (create() then reports failure so callers can fail closed).
class CgroupLimits {
 public:
  bool create(std::size_t memory_mb, std::size_t pids) {
    const std::string parent = current_cgroup_dir();
    if (parent.empty()) {
      return false;
    }
    write_file(parent + "/cgroup.subtree_control", "+memory +pids");
    dir_ = parent + "/cf-sbx-" + std::to_string(::getpid());
    if (::mkdir(dir_.c_str(), 0755) != 0) {
      dir_.clear();
      return false;
    }
    bool ok = true;
    if (memory_mb > 0) {
      ok = write_file(dir_ + "/memory.max", std::to_string(memory_mb * 1024 * 1024)) && ok;
    }
    if (pids > 0) {
      ok = write_file(dir_ + "/pids.max", std::to_string(pids)) && ok;
    }
    if (!ok) {
      remove();
      return false;
    }
    return true;
  }

  bool add(pid_t pid) {
    return !dir_.empty() && write_file(dir_ + "/cgroup.procs", std::to_string(pid));
  }

  void remove() {
    if (dir_.empty()) {
      return;
    }
    write_file(dir_ + "/cgroup.kill", "1");
    ::rmdir(dir_.c_str());
    dir_.clear();
  }

 private:
  std::string dir_;
};

}  // namespace

bool cgroup_delegated() {
  const std::string parent = current_cgroup_dir();
  if (parent.empty() || ::access(parent.c_str(), W_OK) != 0) {
    return false;
  }
  const std::string probe = parent + "/cf-probe-" + std::to_string(::getpid());
  if (::mkdir(probe.c_str(), 0755) != 0) {
    return false;
  }
  ::rmdir(probe.c_str());
  return true;
}

NamespaceExecutor::NamespaceExecutor(NamespaceSandboxSpec spec) : spec_(std::move(spec)) {}

process::ProcessResult NamespaceExecutor::run(const process::ProcessRequest& request) {
  if (request.argv.empty() || request.argv[0].empty()) {
    process::ProcessResult result;
    result.error = "empty argv";
    return result;
  }

  CgroupLimits cgroup;
  const bool cgroup_requested = spec_.memory_mb > 0 || spec_.pids_limit > 0;
  if (cgroup_requested && !cgroup.create(spec_.memory_mb, spec_.pids_limit)) {
    process::ProcessResult result;
    result.error = "cgroup limits requested but no delegated cgroup is available";
    return result;
  }

  Pipe out_pipe;
  Pipe err_pipe;
  Pipe exec_pipe;  // close-on-exec: reports exec/setup failures to the parent
  // Synchronization channel: the child announces its user namespace on its
  // end, the parent answers with the "id maps written" byte on its end. A
  // socketpair keeps the directions apart (a two-way pipe would let the child
  // read back its own announce byte).
  int sync_fds[2] = {-1, -1};
  if (!out_pipe.valid() || !err_pipe.valid() || !exec_pipe.valid() ||
      ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sync_fds) != 0) {
    cgroup.remove();
    process::ProcessResult result;
    result.error = std::string("pipe: ") + std::strerror(errno);
    return result;
  }
  ::fcntl(exec_pipe.write_fd(), F_SETFD, FD_CLOEXEC);

  static std::atomic<unsigned> root_counter{0};
  const std::string sandbox_root =
      "/tmp/cf-ns-" + std::to_string(::getpid()) + "-" +
      std::to_string(root_counter.fetch_add(1));

  const pid_t pid = ::fork();
  if (pid < 0) {
    cgroup.remove();
    process::ProcessResult result;
    result.error = std::string("fork: ") + std::strerror(errno);
    return result;
  }

  if (pid == 0) {
    // Intermediate: enters the namespaces, then forks the command as PID 1
    // of the new pid namespace and mirrors its exit status.
    ::setpgid(0, 0);
    ::dup2(out_pipe.write_fd(), STDOUT_FILENO);
    ::dup2(err_pipe.write_fd(), STDERR_FILENO);
    out_pipe.close_read();
    out_pipe.close_write();
    err_pipe.close_read();
    err_pipe.close_write();
    exec_pipe.close_read();
    ::close(sync_fds[0]);

    if (::unshare(CLONE_NEWUSER) != 0) {
      fail(exec_pipe.write_fd(), errno);
    }
    const char ready = 'u';
    const auto announced = ::write(sync_fds[1], &ready, 1);
    (void)announced;
    char ack = 0;
    if (::read(sync_fds[1], &ack, 1) != 1) {
      fail(exec_pipe.write_fd(), EIO);
    }
    ::close(sync_fds[1]);

    // Switch to the mapped uid/gid (like unshare(1) -r): the kernel maps
    // inside-0 to the invoking user, so the command runs as root in the
    // sandbox while files stay owned by the host user.
    if (::setresgid(0, 0, 0) != 0 || ::setresuid(0, 0, 0) != 0) {
      fail(exec_pipe.write_fd(), errno);
    }
    if (::unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWUTS | CLONE_NEWIPC |
                  CLONE_NEWPID) != 0) {
      fail(exec_pipe.write_fd(), errno);
    }

    const pid_t command = ::fork();
    if (command < 0) {
      fail(exec_pipe.write_fd(), errno);
    }
    if (command > 0) {
      // Closing the command's streams makes the supervisor see EOF as soon as
      // the command exits; exiting as PID 1's parent tears the namespace down.
      ::close(STDOUT_FILENO);
      ::close(STDERR_FILENO);
      exec_pipe.close_write();
      int status = 0;
      while (::waitpid(command, &status, 0) < 0 && errno == EINTR) {
      }
      if (WIFEXITED(status)) {
        ::_exit(WEXITSTATUS(status));
      }
      if (WIFSIGNALED(status)) {
        ::_exit(128 + WTERMSIG(status));
      }
      ::_exit(127);
    }

    // Command: PID 1 of the sandbox, with the confinement stack applied.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      fail(exec_pipe.write_fd(), errno);
    }
    if (setup_root(spec_, sandbox_root) != 0) {
      fail(exec_pipe.write_fd(), errno == 0 ? EPERM : errno);
    }
    if (apply_landlock(spec_) != 0) {
      fail(exec_pipe.write_fd(), errno == 0 ? EPERM : errno);
    }
    if (apply_seccomp() != 0) {
      fail(exec_pipe.write_fd(), errno == 0 ? EPERM : errno);
    }

    ::setenv("HOME", "/home/cf", 1);
    ::setenv("TMPDIR", "/tmp", 1);
    for (const auto& [key, value] : request.environment) {
      ::setenv(key.c_str(), value.c_str(), 1);
    }
    const std::string workdir = host_path_to_container(spec_, request.working_directory);
    if (::chdir(workdir.c_str()) != 0) {
      fail(exec_pipe.write_fd(), errno);
    }
    std::vector<char*> argv;
    argv.reserve(request.argv.size() + 1);
    for (const auto& argument : request.argv) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    fail(exec_pipe.write_fd(), errno);
  }

  out_pipe.close_write();
  err_pipe.close_write();
  exec_pipe.close_write();
  ::close(sync_fds[1]);

  // Map the parent's uid/gid to root inside the child's user namespace. The
  // parent writes the maps (unprivileged single-id mappings are allowed once
  // setgroups is denied); the child waits for the signal before mounting.
  // The read sees EOF if the child dies before announcing.
  char ready = 0;
  if (::read(sync_fds[0], &ready, 1) == 1) {
    const std::string proc = "/proc/" + std::to_string(pid);
    write_file(proc + "/setgroups", "deny");
    write_file(proc + "/uid_map", "0 " + std::to_string(::getuid()) + " 1");
    write_file(proc + "/gid_map", "0 " + std::to_string(::getgid()) + " 1");
    const char ack = 'm';
    const auto signalled = ::send(sync_fds[0], &ack, 1, MSG_NOSIGNAL);
    (void)signalled;
  }
  ::close(sync_fds[0]);

  if (cgroup_requested && !cgroup.add(pid)) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    auto result = process::supervise_child(pid, out_pipe.release_read(), err_pipe.release_read(),
                                           exec_pipe.release_read(), request);
    cgroup.remove();
    remove_root(sandbox_root);
    result.launched = false;
    result.error = "failed to place the sandbox in its cgroup";
    return result;
  }

  auto result = process::supervise_child(pid, out_pipe.release_read(), err_pipe.release_read(),
                                         exec_pipe.release_read(), request);

  cgroup.remove();
  remove_root(sandbox_root);
  return result;
}

}  // namespace cf::sandbox
