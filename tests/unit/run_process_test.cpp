// End-to-end test for the run process (design sections 5.3, 6.6, 7.1).
//
// A fake opencode server stands in for the harness + model + MCP: it serves
// the v1 API the adapter uses, writes the scripted files into the role's
// worktree, and appends the scripted submission to the role's calls file.
// Everything else is real: role configs, server spawning, event wiring,
// session driving, git snapshots, gates and integration.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/common/hash.hpp"
#include "cf/github/client.hpp"
#include "cf/orch/run_process.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::orch::DriverConfig;
using cf::orch::RunProcess;
using cf::orch::RunProcessConfig;
using cf::orch::RunState;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_run_process_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const char* kFakeOpencode = R"python(#!/usr/bin/env python3
import argparse, http.server, json, os, threading

parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("--hostname", default="127.0.0.1")
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

print("fake opencode ready", flush=True)
http.server.HTTPServer((args.hostname, args.port), Handler).serve_forever()
)python";

const char* kScript = R"json({
  "qa": [
    {"files": {"tests/m3_test.cpp": "test\n"},
     "submission": {"tool": "submit_tests",
                    "arguments": {"plan_ref": "qa/plan/M3.yaml", "summary": "tests written"}}},
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
                    "arguments": {"verdict_json": {"verdict": "APPROVED", "summary": "looks good"}}}}
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

TEST(RunProcessTest, DrivesAFullRunToDoneWithFakeOpencode) {
  const auto dir = make_temp_dir();
  cf::process::LocalProcessExecutor executor;

  // Target repository with an initial commit and a bare remote.
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

  // Run store in Executing.
  auto opened = cf::store::RunStore::open(dir / "run.db");
  ASSERT_TRUE(opened.has_value());
  auto store = std::move(*opened);
  cf::store::RunRecord run;
  run.run_id = "R12";
  run.state = "Executing";
  run.created_at = cf::store::unix_now();
  ASSERT_TRUE(store->create_run(run).has_value());

  // Plan, profile, fake opencode and its script.
  cf::plan::Plan plan;
  plan.plan_id = "PLAN-X";
  plan.product = "Y";
  plan.version = "1";
  cf::plan::MilestoneSpec milestone;
  milestone.id = "M3";
  milestone.title = "Registry";
  milestone.requirements = {"REQ-010"};
  plan.milestones.push_back(milestone);
  plan.target.language = "cpp";
  plan.target.standard = "c++20";
  plan.target.build_system = "cmake";
  plan.target.test_framework = "gtest";

  auto profile = cf::gates::load_profile_string(kProfile);
  ASSERT_TRUE(profile.has_value()) << profile.error().message;

  const auto fake_binary = dir / "fake-opencode.py";
  write_file(fake_binary, kFakeOpencode);
  std::filesystem::permissions(fake_binary,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace);
  const auto script_path = dir / "script.json";
  write_file(script_path, kScript);

  RunProcessConfig config;
  config.run_id = "R12";
  config.workspace_root = dir / "workspace";
  config.integration_branch = "cf/R12/integration";
  config.base_branch = "main";
  config.opencode_binary = fake_binary.string();
  config.role_models = {{"coder", "test/coder"}, {"reviewer", "test/reviewer"},
                        {"qa", "test/qa"}};
  config.mcp_command = "cf-mcp";
  config.environment = {{"CF_FAKE_SCRIPT", script_path.string()}};
  config.turn_timeout = std::chrono::seconds(30);
  config.profile = *profile;
  config.profile_language = "cpp";
  config.profile_build_system = "cmake";
  config.profile_test_framework = "gtest";
  config.step_timeout = std::chrono::seconds(30);
  config.orchestrator.escalation_dir =
      (config.workspace_root / "runs" / "R12" / "escalations").string();

  config.in_flight_grace = std::chrono::seconds(0);

  // Operations left behind by an older orchestrator (design sections 6.12.4,
  // 6.12.5): one STARTED past the grace period and one already marked
  // NEEDS_RECONCILE. Startup recovery must reconcile both with the real
  // GitHub handler before the loop issues any new effect.
  const cf::store::UnixTime now = cf::store::unix_now();
  ASSERT_TRUE(store
                  ->acquire_lease("R12", "old-orch", std::chrono::seconds(30),
                                  now - 1000)
                  .has_value());
  std::vector<std::string> recovered_op_ids;
  auto seed_comment = [&](const std::string& body, bool needs_reconcile) {
    cf::store::NewOperation intent;
    intent.idempotency_key =
        cf::idempotency_key("R12", "", "comment", cf::sha256_hex(body));
    intent.kind = "comment";
    intent.input_json =
        nlohmann::json{{"issue_number", 7}, {"body", body}}.dump();
    intent.expected_json = "{}";
    ASSERT_TRUE(store->record_intents("R12", {intent}, now - 1000).has_value());
    auto op = store->find_operation_by_key(intent.idempotency_key);
    ASSERT_TRUE(op.has_value());
    ASSERT_TRUE(store->mark_operation_started(op->op_id, 1, now - 1000).has_value());
    if (needs_reconcile) {
      ASSERT_TRUE(
          store->mark_operation_needs_reconcile(op->op_id, "world differs", now - 1000)
              .has_value());
    }
    recovered_op_ids.push_back(op->op_id);
  };
  seed_comment("stale comment", false);
  seed_comment("needs-reconcile comment", true);

  // GitHub flow: a scripted API and the real bare remote.
  cf::test::ScriptedHttpTransport github;
  // Startup recovery reconciles the two comments: each GET finds the marker.
  for (const auto& op_id : recovered_op_ids) {
    nlohmann::json comments = nlohmann::json::array();
    comments.push_back(
        {{"body", std::string("x\n") + cf::github::Client::op_marker(op_id)}});
    github.script(200, comments.dump());
  }
  github.script(200, "[]");  // PR dedupe: none yet
  github.script(201,
                R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                    "draft": true, "head": {"ref": "cf/R12/integration"},
                    "base": {"ref": "main"}})");
  github.script(200,
                R"({"number": 7, "html_url": "u", "state": "open", "body": "b",
                    "draft": false, "head": {"ref": "cf/R12/integration"},
                    "base": {"ref": "main"}})");
  // Marking the PR ready looks up CODEOWNERS (.github/, root, docs/).
  github.script(404, R"({"message": "Not Found"})");
  github.script(404, R"({"message": "Not Found"})");
  github.script(404, R"({"message": "Not Found"})");
  config.github_transport = &github;
  config.github_token = "ghs_test";
  config.github_owner = "o";
  config.github_repo = "r";

  RunProcess process(*store, plan, config, *repository, executor);
  const auto state = process.execute(cf::store::unix_now());
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, RunState::Done);

  // The integration branch carries the merged work.
  const auto integration = config.workspace_root / "wt" / "R12" / "integration";
  EXPECT_TRUE(std::filesystem::exists(integration / "src" / "m3.cpp"));
  EXPECT_TRUE(std::filesystem::exists(integration / "tests" / "m3_test.cpp"));

  // Role configs, logs and calls files were written.
  const auto run_dir = config.workspace_root / "runs" / "R12";
  EXPECT_TRUE(std::filesystem::exists(run_dir / "opencode-coder.json"));
  EXPECT_TRUE(std::filesystem::exists(run_dir / "opencode-reviewer.json"));
  EXPECT_TRUE(std::filesystem::exists(run_dir / "opencode-qa.json"));
  auto contains = [](const std::filesystem::path& path, const std::string& needle) {
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return text.find(needle) != std::string::npos;
  };
  EXPECT_TRUE(contains(run_dir / "calls-coder.jsonl", "submit_work"));
  EXPECT_TRUE(contains(run_dir / "calls-reviewer.jsonl", "submit_review"));
  EXPECT_TRUE(contains(run_dir / "calls-qa.jsonl", "submit_tests"));
  EXPECT_TRUE(contains(run_dir / "calls-qa.jsonl", "verify_defect"));

  // Gates ran and passed on the milestone's commit.
  auto gates = store->latest_gate_results("R12", "M3");
  ASSERT_TRUE(gates.has_value()) << gates.error().message;
  EXPECT_FALSE(gates->empty());
  for (const auto& row : *gates) {
    EXPECT_TRUE(row.passed) << row.gate;
  }
  const auto stored = store->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->state, "Done");
  EXPECT_EQ(stored->iterations, 1u);

  // GitHub flow: both branches are on the remote, a draft PR was opened and
  // then marked ready when the run reached Done.
  auto milestone_remote = repository->remote_branch_sha("origin", "cf/R12/M3");
  ASSERT_TRUE(milestone_remote.has_value()) << milestone_remote.error().message;
  EXPECT_FALSE(milestone_remote->empty());
  auto integration_remote =
      repository->remote_branch_sha("origin", "cf/R12/integration");
  ASSERT_TRUE(integration_remote.has_value()) << integration_remote.error().message;
  EXPECT_FALSE(integration_remote->empty());

  ASSERT_GE(github.requests().size(), 5u);
  const auto* created_pr = [&]() -> const cf::test::ScriptedHttpTransport::RecordedRequest* {
    for (const auto& request : github.requests()) {
      if (request.method == "POST" &&
          request.url.find("/pulls") != std::string::npos) {
        return &request;
      }
    }
    return nullptr;
  }();
  ASSERT_NE(created_pr, nullptr);
  EXPECT_NE(created_pr->body.find("<!-- cf-op:"), std::string::npos);
  const auto* ready_pr = [&]() -> const cf::test::ScriptedHttpTransport::RecordedRequest* {
    for (const auto& request : github.requests()) {
      if (request.method == "PATCH" &&
          request.body.find("\"draft\":false") != std::string::npos) {
        return &request;
      }
    }
    return nullptr;
  }();
  ASSERT_NE(ready_pr, nullptr);

  // The two operations from the crashed attempt were reconciled, not
  // re-performed or left behind.
  for (const auto& op_id : recovered_op_ids) {
    auto stored_op = store->get_operation(op_id);
    ASSERT_TRUE(stored_op.has_value()) << stored_op.error().message;
    EXPECT_EQ(stored_op->state, cf::store::op_state::kConfirmed);
  }
  std::size_t comment_gets = 0;
  for (const auto& request : github.requests()) {
    if (request.method == "GET" &&
        request.url.find("/issues/7/comments") != std::string::npos) {
      ++comment_gets;
    }
  }
  EXPECT_EQ(comment_gets, 2u);

  auto operations = store->list_operations("R12", {cf::store::op_state::kConfirmed});
  ASSERT_TRUE(operations.has_value());
  std::size_t pushes = 0;
  bool pr_created = false;
  bool pr_updated = false;
  for (const auto& operation : *operations) {
    pushes += operation.kind == "push" ? 1 : 0;
    pr_created = pr_created || operation.kind == "pr_create";
    pr_updated = pr_updated || operation.kind == "pr_update";
  }
  EXPECT_EQ(pushes, 2u);
  EXPECT_TRUE(pr_created);
  EXPECT_TRUE(pr_updated);

  store.reset();
  repository.reset();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
