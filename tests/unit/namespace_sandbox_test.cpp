// Unit tests for the daemon-free namespace executor and its escape suite
// (design sections 6.8.1, 6.8.2, 6.8.4).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sched.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cf/common/process.hpp"
#include "cf/orch/driver.hpp"
#include "cf/sandbox/namespaces.hpp"

namespace {

using cf::process::LocalProcessExecutor;
using cf::process::ProcessRequest;
using cf::process::ProcessResult;
using cf::sandbox::NamespaceExecutor;
using cf::sandbox::NamespaceSandboxSpec;

bool userns_ok() {
  static const bool available = []() {
    const pid_t pid = ::fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      ::_exit(::unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
      return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }();
  return available;
}

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_ns_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

ProcessResult run_in_ns(const NamespaceSandboxSpec& spec, std::vector<std::string> argv,
                        std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
  NamespaceExecutor executor(spec);
  ProcessRequest request;
  request.argv = std::move(argv);
  request.timeout = timeout;
  return executor.run(request);
}

TEST(NamespaceEscapeTest, RunsACommand) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult result = run_in_ns({}, {"/bin/echo", "hello"});
  ASSERT_TRUE(result.launched) << result.error;
  EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_EQ(result.stdout_text, "hello\n");
}

TEST(NamespaceEscapeTest, WorktreeIsReadOnlyExceptTheWritablePaths) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const auto dir = make_temp_dir();
  write_file(dir / "worktree" / "README.md", "x\n");
  std::filesystem::create_directories(dir / "worktree" / "src");

  NamespaceSandboxSpec spec;
  spec.worktree = (dir / "worktree").string();
  spec.writable_paths = {"src"};

  const ProcessResult denied = run_in_ns(spec, {"/bin/sh", "-c", "touch /work/denied.txt"});
  EXPECT_NE(denied.exit_code, 0);
  EXPECT_FALSE(std::filesystem::exists(dir / "worktree" / "denied.txt"));

  const ProcessResult allowed =
      run_in_ns(spec, {"/bin/sh", "-c", "touch /work/src/created.txt"});
  EXPECT_EQ(allowed.exit_code, 0) << allowed.stderr_text;
  EXPECT_TRUE(std::filesystem::exists(dir / "worktree" / "src" / "created.txt"));

  struct stat st {};
  ASSERT_EQ(::stat((dir / "worktree" / "src" / "created.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_uid, ::getuid());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(NamespaceEscapeTest, RootFilesystemIsReadOnly) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult result = run_in_ns({}, {"/bin/sh", "-c", "touch /etc/cf-escape"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(NamespaceEscapeTest, HostPathsAreNotVisible) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const auto dir = make_temp_dir();
  write_file(dir / "secret.txt", "host secret\n");

  const ProcessResult result =
      run_in_ns({}, {"/bin/sh", "-c", "cat " + (dir / "secret.txt").string()});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text.find("host secret"), std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(NamespaceEscapeTest, ExtraReadOnlyMountsAreVisibleAndReadOnly) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const auto dir = make_temp_dir();
  write_file(dir / "cache" / "dep.txt", "cached\n");

  NamespaceSandboxSpec spec;
  spec.extra_readonly_mounts.push_back({(dir / "cache").string(), "/deps"});

  const ProcessResult read = run_in_ns(spec, {"/bin/cat", "/deps/dep.txt"});
  ASSERT_EQ(read.exit_code, 0) << read.stderr_text;
  EXPECT_EQ(read.stdout_text, "cached\n");

  const ProcessResult denied = run_in_ns(spec, {"/bin/sh", "-c", "touch /deps/new.txt"});
  EXPECT_NE(denied.exit_code, 0);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(NamespaceEscapeTest, NetworkIsUnreachable) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult result =
      run_in_ns({}, {"/usr/bin/python3", "-c",
                     "import socket; socket.setdefaulttimeout(3); "
                     "socket.create_connection(('1.1.1.1', 80))"});
  ASSERT_TRUE(result.launched) << result.error;
  EXPECT_NE(result.exit_code, 0);
}

TEST(NamespaceEscapeTest, TmpIsNotExecutable) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult result =
      run_in_ns({}, {"/bin/sh", "-c", "cp /bin/true /tmp/x && /tmp/x"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(NamespaceEscapeTest, BuildDirectoryIsExecutable) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult result =
      run_in_ns({}, {"/bin/sh", "-c", "cp /bin/true /build/x && /build/x"});
  ASSERT_TRUE(result.launched) << result.error;
  EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
}

TEST(NamespaceEscapeTest, WorkingDirectoryMapsIntoTheSandbox) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const auto dir = make_temp_dir();
  std::filesystem::create_directories(dir / "worktree" / "src");

  NamespaceSandboxSpec spec;
  spec.worktree = (dir / "worktree").string();
  NamespaceExecutor executor(spec);
  ProcessRequest request;
  request.argv = {"/bin/pwd"};
  request.working_directory = (dir / "worktree" / "src").string();
  request.timeout = std::chrono::seconds(60);
  const ProcessResult result = executor.run(request);
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_EQ(result.stdout_text, "/work/src\n");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(NamespaceEscapeTest, TimeoutLeavesNoProcessBehind) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult result =
      run_in_ns({}, {"/bin/sh", "-c", "exec sleep 987654"},
                std::chrono::milliseconds(500));
  EXPECT_TRUE(result.timed_out);

  LocalProcessExecutor local;
  ProcessRequest list;
  list.argv = {"/bin/sh", "-c", "pgrep -f 'sleep 987654' || true"};
  list.timeout = std::chrono::seconds(10);
  const ProcessResult processes = local.run(list);
  ASSERT_EQ(processes.exit_code, 0) << processes.stderr_text;
  EXPECT_EQ(processes.stdout_text.find("sleep 987654"), std::string::npos)
      << processes.stdout_text;
}

TEST(NamespaceEscapeTest, ProcIsPrivateToTheSandbox) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const std::string host_pid = std::to_string(::getpid());
  const ProcessResult hidden =
      run_in_ns({}, {"/bin/sh", "-c", "test -e /proc/" + host_pid});
  EXPECT_NE(hidden.exit_code, 0);

  const ProcessResult self =
      run_in_ns({}, {"/bin/sh", "-c", "test -e /proc/self/status"});
  EXPECT_EQ(self.exit_code, 0) << self.stderr_text;
}

TEST(NamespaceEscapeTest, HostProcessesCannotBeSignalled) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const std::string host_pid = std::to_string(::getpid());
  const ProcessResult result = run_in_ns({}, {"/bin/sh", "-c", "kill -0 " + host_pid});
  EXPECT_NE(result.exit_code, 0);
}

TEST(NamespaceEscapeTest, LandlockConfinesFileAccess) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  // The kernel allows a process to write its own oom_score_adj (verified in
  // an equivalent plain userns sandbox); the Landlock read-only rule on
  // /proc denies it with EACCES.
  const ProcessResult result =
      run_in_ns({}, {"/bin/sh", "-c", "echo 100 > /proc/self/oom_score_adj"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(NamespaceEscapeTest, SeccompBlocksDangerousSyscalls) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const ProcessResult unshare = run_in_ns({}, {"/bin/sh", "-c", "unshare -U true"});
  EXPECT_NE(unshare.exit_code, 0);

  const ProcessResult ptrace =
      run_in_ns({}, {"/usr/bin/python3", "-c",
                     "import ctypes; libc = ctypes.CDLL('libc.so.6', use_errno=True); "
                     "libc.ptrace(0, 0, 0, 0); print(ctypes.get_errno())"});
  ASSERT_EQ(ptrace.exit_code, 0) << ptrace.stderr_text;
  EXPECT_EQ(ptrace.stdout_text, "1\n");
}

TEST(NamespaceEscapeTest, CgroupLimitsAreEnforcedWhenDelegated) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  if (!cf::sandbox::cgroup_delegated()) {
    GTEST_SKIP() << "no delegated cgroup is available";
  }
  NamespaceSandboxSpec spec;
  spec.memory_mb = 64;
  const ProcessResult result =
      run_in_ns(spec, {"/usr/bin/python3", "-c",
                       "b = bytearray(256 * 1024 * 1024); print(len(b))"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(NamespaceEscapeTest, CgroupLimitsFailClosedWithoutDelegation) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  if (cf::sandbox::cgroup_delegated()) {
    GTEST_SKIP() << "a delegated cgroup is available";
  }
  NamespaceSandboxSpec spec;
  spec.pids_limit = 16;
  const ProcessResult result = run_in_ns(spec, {"/bin/true"});
  EXPECT_FALSE(result.launched);
  EXPECT_NE(result.error.find("cgroup"), std::string::npos) << result.error;
}

TEST(NamespaceDriverTest, GatesRunInsideTheSandbox) {
  if (!userns_ok()) {
    GTEST_SKIP() << "unprivileged user namespaces are not available";
  }
  const auto dir = make_temp_dir();
  LocalProcessExecutor executor;
  auto created = cf::git::Repository::create(dir / "repo", executor);
  ASSERT_TRUE(created.has_value()) << created.error().message;
  auto repository = std::make_unique<cf::git::Repository>(std::move(*created));
  write_file(dir / "repo" / "README.md", "initial\n");
  cf::git::CommitTrailers init;
  init.run_id = "R13";
  init.op_id = "op_init";
  ASSERT_TRUE(repository
                  ->commit_paths(dir / "repo", {"README.md"}, "chore: init", init)
                  .has_value());

  auto opened = cf::store::RunStore::open(dir / "run.db");
  ASSERT_TRUE(opened.has_value());
  auto store = std::move(*opened);
  cf::store::RunRecord run;
  run.run_id = "R13";
  run.state = "Executing";
  run.created_at = cf::store::unix_now();
  ASSERT_TRUE(store->create_run(run).has_value());

  cf::plan::Plan plan;
  plan.plan_id = "PLAN-Y";
  plan.product = "Z";
  plan.version = "1";
  cf::plan::MilestoneSpec milestone;
  milestone.id = "M4";
  milestone.title = "Registry";
  plan.milestones.push_back(milestone);
  plan.target.language = "cpp";
  plan.target.standard = "c++20";
  plan.target.build_system = "cmake";
  plan.target.test_framework = "gtest";

  auto profile = cf::gates::load_profile_string(R"yaml(
id: namespace-profile
image: python:3.12-slim
languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails: { steps: [[true]] }
    format: { steps: [[true]] }
build_systems:
  cmake: { steps: [[true]] }
test_frameworks:
  gtest: { steps: [[true]], languages: [cpp] }
)yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;

  cf::orch::DriverConfig config;
  config.run_id = "R13";
  config.workspace_root = dir / "workspace";
  config.integration_branch = "cf/R13/integration";
  config.base_branch = "main";
  config.profile = *profile;
  config.profile_language = "cpp";
  config.profile_build_system = "cmake";
  config.profile_test_framework = "gtest";
  config.sandbox_namespaces = true;
  config.step_timeout = std::chrono::seconds(60);

  class FakeRunner final : public cf::orch::AgentRunner {
   public:
    cf::Result<cf::orch::AgentTurnOutcome> run_turn(
        const cf::orch::AgentTurnRequest& request) override {
      if (request.phase == cf::orch::MilestonePhase::AuthorTests) {
        write_file(std::filesystem::path(request.worktree) / "tests" / "t.cpp",
                   "test\n");
      } else if (request.phase == cf::orch::MilestonePhase::Code) {
        write_file(std::filesystem::path(request.worktree) / "src" / "m4.cpp",
                   "code\n");
      }
      cf::orch::AgentTurnOutcome outcome;
      outcome.completed = true;
      outcome.exit_criteria_met = request.phase == cf::orch::MilestonePhase::Validate;
      return outcome;
    }
  };
  FakeRunner runner;

  cf::orch::WorkspaceDriver driver(plan, config, *repository, executor, runner,
                                   store.get());
  cf::orch::StateMachine machine(*store, "R13");
  ASSERT_TRUE(machine.register_milestone("M4").has_value());
  ASSERT_TRUE(machine.apply("M4", cf::orch::MilestoneEvent::Start,
                            cf::store::unix_now())
                  .has_value());
  cf::orch::MilestoneLoop loop(*store, machine, driver);
  const auto state = loop.drive("M4", cf::store::unix_now());
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, cf::orch::MilestoneState::Done);

  auto gates = store->latest_gate_results("R13", "M4");
  ASSERT_TRUE(gates.has_value()) << gates.error().message;
  EXPECT_FALSE(gates->empty());
  for (const auto& row : *gates) {
    EXPECT_TRUE(row.passed) << row.gate;
  }

  store.reset();
  repository.reset();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
