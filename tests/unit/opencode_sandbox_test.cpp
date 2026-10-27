// Tests for sandboxed opencode servers (design section 6.8): the launcher
// flags and an end-to-end run whose agents execute inside a container.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/orch/run_process.hpp"
#include "cf/sandbox/docker.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::orch::OpencodeFactoryConfig;
using cf::orch::RunProcess;
using cf::orch::RunProcessConfig;
using cf::orch::RunState;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_opencode_sandbox_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
  for (const auto& entry : values) {
    if (entry == value) {
      return true;
    }
  }
  return false;
}

TEST(OpencodeSandboxTest, LauncherCarriesTheDesignLayout) {
  OpencodeFactoryConfig config;
  config.run_id = "R12";
  config.run_dir = "/ws/runs/R12";
  config.sandbox_image = "cf-agent-opencode:1.18.35";
  config.sandbox_network = "cf-model-net";
  config.mcp_binary_host = "/opt/cf/cf-mcp";
  config.sandbox_proxy_url = "http://host.docker.internal:3128";
  config.environment = {{"CF_FAKE_SCRIPT", "/cf/script.json"}};

  const auto argv = cf::orch::sandboxed_server_launcher(
      config, "coder", "/ws/wt/R12/M3/coder", 4321, "/ws/runs/R12/opencode-coder.json",
      "/ws/runs/R12/calls-coder.jsonl");

  EXPECT_TRUE(contains(argv, "run"));
  EXPECT_TRUE(contains(argv, "--rm"));
  EXPECT_TRUE(contains(argv, "--read-only"));
  EXPECT_TRUE(contains(argv, "cf-oc-coder-4321"));
  EXPECT_TRUE(contains(argv, "127.0.0.1:4321:4321"));
  EXPECT_TRUE(contains(argv, "cf-model-net"));
  EXPECT_TRUE(contains(argv, "--cap-drop"));
  EXPECT_TRUE(contains(argv, "ALL"));
  EXPECT_TRUE(contains(argv, "no-new-privileges"));
  EXPECT_TRUE(contains(argv, "--label"));
  EXPECT_TRUE(contains(argv, "cf.run=R12"));
  EXPECT_TRUE(contains(argv, "cf.kind=agent"));
  EXPECT_TRUE(contains(argv, "--workdir"));
  EXPECT_TRUE(contains(argv, "/work"));

  EXPECT_TRUE(contains(argv, "/ws/wt/R12/M3/coder:/work:ro"));
  EXPECT_TRUE(contains(argv, "/ws/wt/R12/M3/coder/src:/work/src"));
  EXPECT_TRUE(contains(argv, "/ws/wt/R12/M3/coder/include:/work/include"));
  EXPECT_TRUE(contains(argv, "/ws/wt/R12/M3/coder/.git:/work/.git:ro"));
  EXPECT_TRUE(contains(argv, "/ws/runs/R12:/cf"));
  EXPECT_TRUE(contains(argv, "/opt/cf/cf-mcp:/usr/local/bin/cf-mcp:ro"));
  EXPECT_TRUE(contains(argv, "/tmp:rw,noexec,nosuid,nodev,size=64m"));
  EXPECT_TRUE(contains(argv, "/build:rw,exec,nosuid,nodev,size=1g"));

  EXPECT_TRUE(contains(argv, "HOME=/home/cf"));
  EXPECT_TRUE(contains(argv, "OPENCODE_CONFIG=/cf/opencode-coder.json"));
  EXPECT_TRUE(contains(argv, "CF_MCP_CALLS_FILE=/cf/calls-coder.jsonl"));
  EXPECT_TRUE(contains(argv, "CF_ROLE=coder"));
  EXPECT_TRUE(contains(argv, "CF_FAKE_SCRIPT=/cf/script.json"));
  // The egress proxy is reachable and used for all traffic.
  EXPECT_TRUE(contains(argv, "host.docker.internal:host-gateway"));
  EXPECT_TRUE(contains(argv, "HTTP_PROXY=http://host.docker.internal:3128"));
  EXPECT_TRUE(contains(argv, "HTTPS_PROXY=http://host.docker.internal:3128"));
  EXPECT_TRUE(contains(argv, "NO_PROXY=localhost,127.0.0.1"));
  EXPECT_EQ(argv.back(), "cf-agent-opencode:1.18.35");
}

TEST(OpencodeSandboxTest, ReviewerGetsNoWritableMounts) {
  OpencodeFactoryConfig config;
  config.run_id = "R12";
  config.run_dir = "/ws/runs/R12";
  config.sandbox_image = "img";
  const auto argv = cf::orch::sandboxed_server_launcher(
      config, "reviewer", "/ws/wt/R12/M3/review", 5000, "/ws/runs/R12/c.json",
      "/ws/runs/R12/calls-reviewer.jsonl");
  EXPECT_TRUE(contains(argv, "/ws/wt/R12/M3/review:/work:ro"));
  EXPECT_FALSE(contains(argv, "/ws/wt/R12/M3/review/src:/work/src"));
  EXPECT_TRUE(contains(argv, "bridge"));  // default: publishing needs a bridge
}

TEST(OpencodeSandboxTest, InternalNetworkModeSkipsPublishing) {
  OpencodeFactoryConfig config;
  config.run_id = "R12";
  config.run_dir = "/ws/runs/R12";
  config.sandbox_image = "img";
  config.sandbox_network = "cf-sandbox";
  config.sandbox_internal_network = true;
  config.sandbox_proxy_url = "http://172.21.0.1:3128";
  const auto argv = cf::orch::sandboxed_server_launcher(
      config, "coder", "/ws/wt/R12/M3/coder", 5000, "/ws/runs/R12/c.json",
      "/ws/runs/R12/calls-coder.jsonl");
  EXPECT_FALSE(contains(argv, "-p"));  // internal networks cannot publish
  EXPECT_FALSE(contains(argv, "host.docker.internal:host-gateway"));
  EXPECT_TRUE(contains(argv, "cf-sandbox"));
  EXPECT_TRUE(contains(argv, "HTTP_PROXY=http://172.21.0.1:3128"));
  EXPECT_TRUE(contains(argv, "HTTPS_PROXY=http://172.21.0.1:3128"));
}

const char* kFakeOpencode = R"python(#!/usr/bin/env python3
import argparse, http.server, json, os, threading

parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("--hostname", default="0.0.0.0")
parser.add_argument("--port", type=int, required=True)
args = parser.parse_args()

ROLE = os.environ.get("CF_ROLE", "coder")
SCRIPT = os.environ.get("CF_FAKE_SCRIPT", "")
CALLS = os.environ.get("CF_MCP_CALLS_FILE", "")
entries = {}
if SCRIPT and os.path.exists(SCRIPT):
    with open(SCRIPT) as handle:
        entries = json.load(handle).get(ROLE, [])
lock = threading.Lock()

def next_index():
    if not CALLS or not os.path.exists(CALLS):
        return 0
    with open(CALLS) as handle:
        return sum(1 for line in handle if line.strip())

class Handler(http.server.BaseHTTPRequestHandler):
    def _json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/global/health":
            return self._json(200, {"healthy": True, "version": "1.18.35"})
        if self.path == "/session/status":
            return self._json(200, {"ses_fake": {"type": "idle"}})
        if self.path == "/permission":
            return self._json(200, [])
        return self._json(404, {})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        if self.path == "/session":
            return self._json(200, {"id": "ses_fake"})
        if self.path.endswith("/prompt_async"):
            with lock:
                index = next_index()
                if index < len(entries):
                    entry = entries[index]
                    for relative, content in entry.get("files", {}).items():
                        full = os.path.join(os.getcwd(), relative)
                        os.makedirs(os.path.dirname(full), exist_ok=True)
                        with open(full, "w") as handle:
                            handle.write(content)
                    if "submission" in entry and CALLS:
                        with open(CALLS, "a") as handle:
                            handle.write(json.dumps(entry["submission"]) + "\n")
            self.send_response(204)
            self.end_headers()
            return
        self.send_response(200)
        self.end_headers()

    def log_message(self, *a):
        pass

print("fake opencode in container", flush=True)
http.server.HTTPServer((args.hostname, args.port), Handler).serve_forever()
)python";

const char* kScript = R"json({
  "qa": [
    {"files": {"tests/m3_test.cpp": "test\n"},
     "submission": {"tool": "submit_tests",
                    "arguments": {"plan_ref": "qa/plan/M3.yaml", "summary": "tests"}}},
    {"submission": {"tool": "verify_defect",
                    "arguments": {"id": "D-1", "result": "VERIFIED"}}},
    {"submission": {"tool": "verify_defect",
                    "arguments": {"id": "D-1", "result": "VERIFIED"}}}
  ],
  "coder": [
    {"files": {"src/m3.cpp": "int m3() { return 0; }\n"},
     "submission": {"tool": "submit_work",
                    "arguments": {"summary": "implemented", "defect_ids": []}}}
  ],
  "reviewer": [
    {"submission": {"tool": "submit_review",
                    "arguments": {"verdict_json": {"verdict": "APPROVED", "summary": "ok"}}}}
  ]
})json";

const char* kProfile = R"yaml(
id: run-profile
image: test-image
languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails: { steps: [[true]] }
    format: { steps: [[true]] }
build_systems:
  cmake:
    steps: [[true]]
    agent_commands: [cmake, ninja, ctest]
test_frameworks:
  gtest: { steps: [[true]], languages: [cpp] }
)yaml";

TEST(OpencodeSandboxTest, SandboxedRunProcessReachesDone) {
  if (!cf::sandbox::docker_available()) {
    GTEST_SKIP() << "docker is not available";
  }
  const auto dir = make_temp_dir();
  cf::process::LocalProcessExecutor executor;

  // Target repository with an initial commit and a bare remote.
  auto created = cf::git::Repository::create(dir / "repo", executor);
  ASSERT_TRUE(created.has_value()) << created.error().message;
  auto repository = std::make_unique<cf::git::Repository>(std::move(*created));
  write_file(dir / "repo" / "README.md", "initial\n");
  cf::git::CommitTrailers init;
  init.run_id = "R12";
  init.milestone_id = "M1";
  init.op_id = "op_init";
  ASSERT_TRUE(repository
                  ->commit_paths(dir / "repo", {"README.md"}, "chore: init", init)
                  .has_value());
  {
    cf::process::ProcessRequest bare;
    bare.argv = {"git", "init", "--bare", (dir / "remote.git").string()};
    bare.timeout = std::chrono::seconds(30);
    const auto result = executor.run(bare);
    ASSERT_TRUE(result.launched && result.exit_code == 0) << result.stderr_text;
    ASSERT_TRUE(repository
                    ->command({"remote", "add", "origin",
                               (dir / "remote.git").string()})
                    .has_value());
  }

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

  auto profile = cf::gates::load_profile_string(kProfile);
  ASSERT_TRUE(profile.has_value()) << profile.error().message;

  // The fake opencode and its script live in the run directory, which the
  // sandbox mounts at /cf.
  const auto run_dir = dir / "workspace" / "runs" / "R12";
  std::filesystem::create_directories(run_dir);
  const auto fake_binary = run_dir / "fake-opencode";
  write_file(fake_binary, kFakeOpencode);
  std::filesystem::permissions(fake_binary,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace);
  write_file(run_dir / "script.json", kScript);

  RunProcessConfig config;
  config.run_id = "R12";
  config.workspace_root = dir / "workspace";
  config.integration_branch = "cf/R12/integration";
  config.base_branch = "main";
  config.opencode_binary = "unused";
  config.role_models = {{"coder", "test/coder"}, {"reviewer", "test/reviewer"},
                        {"qa", "test/qa"}};
  config.mcp_command = "cf-mcp";
  config.environment = {{"CF_FAKE_SCRIPT", "/cf/script.json"}};
  config.turn_timeout = std::chrono::seconds(60);
  config.profile = *profile;
  config.profile_language = "cpp";
  config.profile_build_system = "cmake";
  config.profile_test_framework = "gtest";
  config.step_timeout = std::chrono::seconds(60);
  config.orchestrator.escalation_dir = (run_dir / "escalations").string();
  // The agents run in the sandbox; no network is needed for the fake. The
  // factory creates this network as internal: no route out of the sandbox.
  const std::string network_name =
      "cf-sbx-test-" + std::to_string(::getpid());
  config.agent_sandbox_image = "python:3.12-slim";
  config.agent_sandbox_binary = "/cf/fake-opencode";
  config.agent_sandbox_network = network_name;

  cf::test::ScriptedHttpTransport github;
  github.script(200, "[]");
  github.script(201,
                R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                    "draft": true, "head": {"ref": "cf/R12/integration"},
                    "base": {"ref": "main"}})");
  github.script(200,
                R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                    "draft": false, "head": {"ref": "cf/R12/integration"},
                    "base": {"ref": "main"}})");
  config.github_transport = &github;
  config.github_token = "ghs_test";
  config.github_owner = "o";
  config.github_repo = "r";
  config.export_defects = false;

  RunProcess process(*store, plan, config, *repository, executor);
  const auto state = process.execute(cf::store::unix_now());
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Done);

  // The container wrote into the mounted worktrees and the run directory.
  const auto integration = config.workspace_root / "wt" / "R12" / "integration";
  EXPECT_TRUE(std::filesystem::exists(integration / "src" / "m3.cpp"));
  EXPECT_TRUE(std::filesystem::exists(integration / "tests" / "m3_test.cpp"));
  auto calls = std::ifstream(run_dir / "calls-coder.jsonl");
  const std::string calls_text((std::istreambuf_iterator<char>(calls)),
                               std::istreambuf_iterator<char>());
  EXPECT_NE(calls_text.find("submit_work"), std::string::npos);

  // No sandboxed server is left behind.
  cf::process::ProcessRequest list;
  list.argv = {"docker", "ps", "-a", "--filter", "name=cf-oc-", "--format",
               "{{.Names}}"};
  list.timeout = std::chrono::seconds(10);
  const auto containers = executor.run(list);
  ASSERT_EQ(containers.exit_code, 0) << containers.stderr_text;
  EXPECT_EQ(containers.stdout_text.find("cf-oc-"), std::string::npos)
      << containers.stdout_text;

  // Egress from the internal sandbox network is kernel-blocked: a direct
  // connection to the internet fails even without the proxy.
  cf::process::ProcessRequest probe;
  probe.argv = {"docker", "run", "--rm", "--network", network_name,
                "python:3.12-slim", "python3", "-c",
                "import socket; socket.setdefaulttimeout(2); "
                "socket.create_connection(('1.1.1.1', 80))"};
  probe.timeout = std::chrono::seconds(60);
  const auto probe_result = executor.run(probe);
  EXPECT_NE(probe_result.exit_code, 0);

  // The network is left behind by design (shared across runs); remove the
  // test's own network.
  cf::process::ProcessRequest remove;
  remove.argv = {"docker", "network", "rm", network_name};
  remove.timeout = std::chrono::seconds(30);
  (void)executor.run(remove);

  store.reset();
  repository.reset();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
