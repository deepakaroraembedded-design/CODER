// Unit tests for the Docker sandbox executor and the escape suite (design
// sections 6.8.1, 6.8.2, 6.8.4).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/orch/driver.hpp"
#include "cf/sandbox/docker.hpp"

namespace {

using cf::process::ProcessRequest;
using cf::process::ProcessResult;
using cf::sandbox::DockerSandboxExecutor;
using cf::sandbox::SandboxSpec;

// Small image with sh, true and python3; present locally and pullable in CI.
constexpr const char* kTestImage = "python:3.12-slim";

bool docker_ok() {
  static const bool available = cf::sandbox::docker_available();
  return available;
}

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_sandbox_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

ProcessResult run_in_sandbox(const SandboxSpec& spec, std::vector<std::string> argv,
                             std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
  DockerSandboxExecutor executor(spec);
  ProcessRequest request;
  request.argv = std::move(argv);
  request.timeout = timeout;
  return executor.run(request);
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

TEST(SandboxArgvTest, HardeningFlagsAndMountLayout) {
  SandboxSpec spec;
  spec.image = "img";
  spec.worktree = "/host/wt";
  spec.writable_paths = {"src", "include"};
  spec.extra_mounts.push_back(cf::sandbox::SandboxMount{"/host/deps", "/deps", false});
  spec.cpus = 2.0;
  spec.memory_mb = 256;
  spec.pids_limit = 64;

  ProcessRequest request;
  request.argv = {"cmake", "--build", "/build"};
  request.environment = {{"CF_X", "1"}};

  const auto argv = cf::sandbox::docker_run_argv(spec, request, "cf-sandbox-1");
  EXPECT_TRUE(contains(argv, "run"));
  EXPECT_TRUE(contains(argv, "--rm"));
  EXPECT_TRUE(contains(argv, "--read-only"));
  EXPECT_TRUE(contains(argv, "--name"));
  EXPECT_TRUE(contains(argv, "cf-sandbox-1"));
  EXPECT_TRUE(contains(argv, "--network"));
  EXPECT_TRUE(contains(argv, "none"));
  EXPECT_TRUE(contains(argv, "--cap-drop"));
  EXPECT_TRUE(contains(argv, "ALL"));
  EXPECT_TRUE(contains(argv, "--security-opt"));
  EXPECT_TRUE(contains(argv, "no-new-privileges"));
  EXPECT_TRUE(contains(argv, std::to_string(::getuid()) + ":" + std::to_string(::getgid())));
  EXPECT_TRUE(contains(argv, "/host/wt:/work:ro"));
  EXPECT_TRUE(contains(argv, "/host/wt/src:/work/src"));
  EXPECT_TRUE(contains(argv, "/host/wt/include:/work/include"));
  EXPECT_TRUE(contains(argv, "/host/wt/.git:/work/.git:ro"));
  EXPECT_TRUE(contains(argv, "/host/deps:/deps:ro"));
  EXPECT_TRUE(contains(argv, "/tmp:rw,noexec,nosuid,nodev,size=64m"));
  EXPECT_TRUE(contains(argv, "/build:rw,exec,nosuid,nodev,size=1g"));
  EXPECT_TRUE(contains(argv, "HOME=/home/cf"));
  EXPECT_TRUE(contains(argv, "CF_X=1"));
  EXPECT_TRUE(contains(argv, "256m"));
  EXPECT_TRUE(contains(argv, "64"));
  EXPECT_TRUE(contains(argv, "2.000000"));

  ASSERT_GE(argv.size(), 4u);
  EXPECT_EQ(argv[argv.size() - 4], "img");
  EXPECT_EQ(argv[argv.size() - 3], "cmake");
  EXPECT_EQ(argv[argv.size() - 2], "--build");
  EXPECT_EQ(argv[argv.size() - 1], "/build");
}

TEST(SandboxArgvTest, NetworkAndCustomUserOptions) {
  SandboxSpec spec;
  spec.image = "img";
  spec.network = true;
  spec.network_name = "cf-model-net";
  spec.user = "1234:5678";
  ProcessRequest request;
  request.argv = {"true"};
  const auto argv = cf::sandbox::docker_run_argv(spec, request);
  EXPECT_TRUE(contains(argv, "cf-model-net"));
  EXPECT_TRUE(contains(argv, "1234:5678"));
  EXPECT_FALSE(contains(argv, "--name"));
}

TEST(SandboxEscapeTest, RunsACommandInTheContainer) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result =
      run_in_sandbox(spec, {"python3", "-c", "print('hello')"});
  EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_NE(result.stdout_text.find("hello"), std::string::npos);
}

TEST(SandboxEscapeTest, WorktreeIsReadOnlyExceptTheWritablePaths) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  const auto dir = make_temp_dir();
  write_file(dir / "worktree" / "README.md", "x\n");
  std::filesystem::create_directories(dir / "worktree" / "src");

  SandboxSpec spec;
  spec.image = kTestImage;
  spec.worktree = (dir / "worktree").string();
  spec.writable_paths = {"src"};

  const ProcessResult denied =
      run_in_sandbox(spec, {"sh", "-c", "touch /work/denied.txt"});
  EXPECT_NE(denied.exit_code, 0);
  EXPECT_FALSE(std::filesystem::exists(dir / "worktree" / "denied.txt"));

  const ProcessResult allowed =
      run_in_sandbox(spec, {"sh", "-c", "touch /work/src/created.txt"});
  EXPECT_EQ(allowed.exit_code, 0) << allowed.stderr_text;
  EXPECT_TRUE(std::filesystem::exists(dir / "worktree" / "src" / "created.txt"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(SandboxEscapeTest, RootFilesystemIsReadOnly) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result =
      run_in_sandbox(spec, {"sh", "-c", "touch /etc/cf-escape"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(SandboxEscapeTest, HostPathsAreNotVisible) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  const auto dir = make_temp_dir();
  write_file(dir / "secret.txt", "host secret\n");

  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result =
      run_in_sandbox(spec, {"sh", "-c", "cat " + (dir / "secret.txt").string()});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text.find("host secret"), std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(SandboxEscapeTest, NetworkIsUnreachable) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result = run_in_sandbox(
      spec, {"python3", "-c",
             "import socket; socket.setdefaulttimeout(3); "
             "socket.create_connection(('1.1.1.1', 80))"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(SandboxEscapeTest, TmpIsNotExecutable) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result =
      run_in_sandbox(spec, {"sh", "-c", "cp /bin/true /tmp/x && /tmp/x"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(SandboxEscapeTest, RunsAsTheConfiguredUser) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result = run_in_sandbox(spec, {"id", "-u"});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_EQ(result.stdout_text, std::to_string(::getuid()) + "\n");
}

TEST(SandboxEscapeTest, MemoryLimitIsEnforced) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  spec.memory_mb = 64;
  const ProcessResult result = run_in_sandbox(
      spec, {"python3", "-c", "b = bytearray(256 * 1024 * 1024)"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(SandboxEscapeTest, TimeoutLeavesNoContainerBehind) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  SandboxSpec spec;
  spec.image = kTestImage;
  const ProcessResult result =
      run_in_sandbox(spec, {"sleep", "30"}, std::chrono::milliseconds(500));
  EXPECT_TRUE(result.timed_out);

  cf::process::LocalProcessExecutor local;
  ProcessRequest list;
  list.argv = {"docker", "ps", "-a", "--filter", "name=cf-sandbox-", "--format",
               "{{.Names}}"};
  list.timeout = std::chrono::seconds(10);
  const ProcessResult containers = local.run(list);
  ASSERT_EQ(containers.exit_code, 0) << containers.stderr_text;
  EXPECT_EQ(containers.stdout_text.find("cf-sandbox-"), std::string::npos)
      << containers.stdout_text;
}

TEST(SandboxDriverTest, GatesRunInsideTheSandbox) {
  if (!docker_ok()) {
    GTEST_SKIP() << "docker is not available";
  }
  const auto dir = make_temp_dir();
  cf::process::LocalProcessExecutor executor;
  auto created = cf::git::Repository::create(dir / "repo", executor);
  ASSERT_TRUE(created.has_value()) << created.error().message;
  auto repository = std::make_unique<cf::git::Repository>(std::move(*created));
  write_file(dir / "repo" / "README.md", "initial\n");
  cf::git::CommitTrailers init;
  init.run_id = "R12";
  init.op_id = "op_init";
  ASSERT_TRUE(repository
                  ->commit_paths(dir / "repo", {"README.md"}, "chore: init", init)
                  .has_value());

  auto opened = cf::store::RunStore::open(dir / "run.db");
  ASSERT_TRUE(opened.has_value());
  auto store = std::move(*opened);
  cf::store::RunRecord run;
  run.run_id = "R12";
  run.state = "Executing";
  run.created_at = cf::store::unix_now();
  ASSERT_TRUE(store->create_run(run).has_value());

  cf::plan::Plan plan;
  plan.plan_id = "PLAN-X";
  plan.product = "Y";
  plan.version = "1";
  cf::plan::MilestoneSpec milestone;
  milestone.id = "M3";
  milestone.title = "Registry";
  plan.milestones.push_back(milestone);
  plan.target.language = "cpp";
  plan.target.standard = "c++20";
  plan.target.build_system = "cmake";
  plan.target.test_framework = "gtest";

  auto profile = cf::gates::load_profile_string(R"yaml(
id: sandbox-profile
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
  config.run_id = "R12";
  config.workspace_root = dir / "workspace";
  config.integration_branch = "cf/R12/integration";
  config.base_branch = "main";
  config.profile = *profile;
  config.profile_language = "cpp";
  config.profile_build_system = "cmake";
  config.profile_test_framework = "gtest";
  config.sandbox_image = kTestImage;
  config.step_timeout = std::chrono::seconds(60);

  class FakeRunner final : public cf::orch::AgentRunner {
   public:
    cf::Result<cf::orch::AgentTurnOutcome> run_turn(
        const cf::orch::AgentTurnRequest& request) override {
      if (request.phase == cf::orch::MilestonePhase::AuthorTests) {
        write_file(std::filesystem::path(request.worktree) / "tests" / "t.cpp",
                   "test\n");
      } else if (request.phase == cf::orch::MilestonePhase::Code) {
        write_file(std::filesystem::path(request.worktree) / "src" / "m3.cpp",
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
  cf::orch::StateMachine machine(*store, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  ASSERT_TRUE(machine.apply("M3", cf::orch::MilestoneEvent::Start,
                            cf::store::unix_now())
                  .has_value());
  cf::orch::MilestoneLoop loop(*store, machine, driver);
  const auto state = loop.drive("M3", cf::store::unix_now());
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, cf::orch::MilestoneState::Done);

  auto gates = store->latest_gate_results("R12", "M3");
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
