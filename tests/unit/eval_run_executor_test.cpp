// End-to-end test for the benchmark run executor (design sections 17.2,
// 18.2 Phase 0b).
//
// A fake opencode server stands in for the harness + model + MCP (same
// pattern as run_process_test): everything else is real — scratch repository,
// run store, orchestrator, snapshots, gates, integration, hidden tests.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/eval/run_executor.hpp"
#include "cf/eval/suite.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_run_executor_test_" + std::to_string(::getpid()) + "_" +
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
    {"files": {"tests/m1_test.cpp": "test\n"},
     "submission": {"tool": "submit_tests",
                    "arguments": {"plan_ref": "qa/plan/M1.yaml", "summary": "tests written"}}},
    {"submission": {"tool": "verify_defect",
                    "arguments": {"id": "D-1", "result": "VERIFIED"}}},
    {"submission": {"tool": "verify_defect",
                    "arguments": {"id": "D-1", "result": "VERIFIED"}}}
  ],
  "coder": [
    {"files": {"include/answer.hpp": "#pragma once\ninline int answer() { return 42; }\n"},
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

const char* kPlan = R"yaml(
plan_id: GOLD-TEST-ANSWER
product: golden
version: 1.0
milestones:
  - id: M1
    title: Answer component
    requirements: [REQ-A-1]
    acceptance:
      - "answer() returns 42"
target:
  language: cpp
  standard: c++20
  build_system: cmake
  test_framework: gtest
inference: fireworks
)yaml";

const char* kHiddenSpec = R"yaml(
compile:
  - name: build
    argv: [g++, -std=c++17, -I, "{worktree}/include", "{hidden}/answer_test.cpp", -o, "{build}/answer_test"]
tests:
  - name: answer
    argv: ["{build}/answer_test"]
)yaml";

const char* kAnswerTest = R"cpp(
#include <cstdio>
#include "answer.hpp"
int main() {
  const int ok = answer() == 42 ? 1 : 0;
  std::printf("{\"tests\": 1, \"passed\": %d}\n", ok);
  return ok == 1 ? 0 : 1;
}
)cpp";

TEST(EvalRunExecutorTest, DrivesAPlanThroughTheRealPipeline) {
  const auto dir = make_temp_dir();
  const auto suite_dir = dir / "eval";
  write_file(suite_dir / "plans" / "answer.yaml", kPlan);
  write_file(suite_dir / "hidden" / "answer" / "spec.yaml", kHiddenSpec);
  write_file(suite_dir / "hidden" / "answer" / "answer_test.cpp", kAnswerTest);
  write_file(suite_dir / "suite.yaml", R"yaml(
suite: test-suite
repeats: 1
plans:
  - id: answer
    category: functional
    language: cpp
    tier: T1
    plan: plans/answer.yaml
)yaml");
  write_file(dir / "profiles" / "run-profile.yaml", kProfile);

  const auto fake_binary = dir / "fake-opencode.py";
  write_file(fake_binary, kFakeOpencode);
  std::filesystem::permissions(fake_binary,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace);
  const auto script_path = dir / "script.json";
  write_file(script_path, kScript);

  auto manifest = cf::eval::load_suite_manifest(suite_dir / "suite.yaml");
  ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
  ASSERT_EQ(manifest->plans.size(), 1u);

  cf::eval::RunExecutorOptions options;
  // Relative on purpose: the executor anchors the workspace so git worktrees
  // and opencode servers, which run with other working directories, resolve
  // it correctly (regression for relative --workspace paths).
  options.workspace_root =
      std::filesystem::relative(dir / "bench-work", std::filesystem::current_path());
  ASSERT_FALSE(options.workspace_root.is_absolute());
  options.suite_dir = manifest->base_dir;
  options.profiles_dir = dir / "profiles";
  options.hidden_dir = suite_dir / "hidden";
  options.opencode_binary = fake_binary.string();
  options.mcp_binary = "cf-mcp";
  options.role_models = {{"coder", "test/coder"},
                         {"reviewer", "test/reviewer"},
                         {"qa", "test/qa"}};
  options.environment = {{"CF_FAKE_SCRIPT", script_path.string()}};
  options.turn_timeout = std::chrono::seconds(30);
  options.step_timeout = std::chrono::seconds(30);

  cf::process::LocalProcessExecutor executor;
  cf::eval::RunProcessExecutor plan_executor(std::move(options), executor);
  auto outcome = plan_executor.run(manifest->plans[0], 0);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().message;

  EXPECT_EQ(outcome->plan_id, "answer");
  EXPECT_TRUE(outcome->terminal);
  EXPECT_EQ(outcome->milestones_total, 1);
  EXPECT_EQ(outcome->milestones_done, 1);
  EXPECT_EQ(outcome->iterations, 1);
  EXPECT_EQ(outcome->hidden_tests_total, 1);
  EXPECT_EQ(outcome->hidden_tests_passed, 1);
  EXPECT_GE(outcome->duration_s, 0.0);

  // The scratch repository and the integration worktree hold the merged work.
  const std::filesystem::path scratch = plan_executor.scratch_directory("answer", 0);
  const std::string run_id = cf::eval::RunProcessExecutor::run_id_for("answer", 0);
  EXPECT_TRUE(std::filesystem::exists(scratch / "repo" / "README.md"));
  EXPECT_TRUE(std::filesystem::exists(scratch / "wt" / run_id / "integration" /
                                      "include" / "answer.hpp"));

  // A second repeat gets its own scratch directory and run id.
  EXPECT_EQ(cf::eval::RunProcessExecutor::run_id_for("answer", 1),
            "bench-answer-r1");
  EXPECT_NE(plan_executor.scratch_directory("answer", 0),
            plan_executor.scratch_directory("answer", 1));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
