# Coder Framework (CF)

Multi-agent, guardrail-driven code generation. CF takes an approved implementation
plan ("Agentic AI multicoder X for Product Y") and turns it into reviewed, tested,
policy-compliant code in the language the plan specifies, with little human
involvement. The framework itself — orchestrator, Coder, Reviewer and QA agents — is
written in **strict C++17** (no GNU extensions, no C++20).

The full specification is [`Coder_Framework_Design_Document.md`](Coder_Framework_Design_Document.md)
(v1.8). This README covers the implemented system, how to build and run it, and what
is still missing.

---

## 1. Status

| Area | State |
|---|---|
| Phase 0a — opencode harness go/no-go | **Complete**: 300/300 iterations passed (H1–H15 × 20) against pinned opencode 1.18.35; recommendation **Go** (`docs/phase0a/decision.md`) |
| Run store, side-effect protocol, crash recovery | Implemented and tested |
| State machines, scheduler, review rules, milestone loop, run orchestrator | Implemented and tested; refined plans' declared write sets are enforced at snapshot time (§6.13.2), and a merge that changes a public interface revalidates the Done milestones that use it against the new integration head (§6.13.3; a revalidation fix starts from that head) |
| Toolchain profiles and staged gate runner | Implemented and tested |
| RAG retrieval (BM25 + vectors, precedence, conflicts) and document ingestion | Implemented and tested |
| MCP tool surface (role-filtered, schema-validated) | Implemented and tested |
| Reference documentation mirror (`cf-refmirror`) | Implemented and tested |
| Git layer (worktrees, trailers, diffs, squash merges) | Implemented and tested |
| Workspace milestone driver (agents + git + gates) | Implemented and tested; fix turns receive the milestone's open defects and every fix turn counts one attempt per defect (escalates at 4, §12) |
| opencode-backed `AgentRunner` | Implemented end to end: turn driving with event-driven (`session.idle`) turn completion, permission broker, event routing, role configs, versioned prompt templates, RAG-backed question answering, server spawning, dead-server detection and restart before the next turn (§6.12, H8), `RunProcess` and `cf run --execute` |
| GitHub operations | Complete lifecycle: client (PRs, issues, comments, `cf-op` + fingerprint markers), pre-push checks (branch policy, protected paths, size, binaries, trailers, secret scanning, dependency licences), dispatcher handlers (`push`, `pr_create`, `pr_update` incl. close, `issue_create` with fingerprint de-dupe, `issue_update`, `comment`), `GithubFlow` (milestone pushes, draft PR open/update/ready, escalation branch + comment, abort closes the PR), defect export to GitHub Issues (`DefectExporter`: one issue per defect with labels, status sync, human closure → WONT_FIX, secret-scanning and restricted-run redaction), GitHub App auth (RS256 JWT → short-lived installation tokens) and CLI wiring. Marking the PR ready requests the CODEOWNERS reviewers (`@user` → reviewer, `@org/team` → team reviewer; GitHub's `.github/`, root and `docs/` lookup order; absent file is fine; repeated on reconcile, which is idempotent) |
| CLI (`cf check/run/status/resume/escalations/report`) | Implemented and tested |
| Design refinement (Phase 4) | Implemented and verified with the real planner model: the refiner turns the plan into a design package (`runs/<run>/design.{json,md}` — component design, refined milestone DAG with write sets/uses/interfaces for the scheduler, ADRs), self-critiques and revises once, stops at the human approval gate, and the approved package drives execution. Approving publishes the design as a product-scoped RAG design document (`<workspace>/designs/`, status `approved`; an earlier design for the same plan becomes `superseded` and is no longer retrieved), and the run's MCP servers and question policy carry the product/language applicability context. The curated pattern catalog (`catalog/patterns/`, six entries) is loaded by `cf-mcp --patterns`, is authoritative for `get_pattern` and is indexed into retrieval; the refiner prompt carries it. Remaining: pattern-conformance checks, adversarial conflict set |
| Golden evaluation suite (`cf-eval`) | Implemented and tested: suite validation, scoring with confidence intervals and target checks, RAG retrieval evaluation, hidden acceptance tests, and the Phase 0b model benchmark (`cf-eval bench`) that runs candidates through the real run pipeline and ranks them; the suite spans C++, Python and Go plus a crash-injection reliability scenario, and the full benchmark (3 candidates × 7 plans) is complete and confirms the pinned models |
| Sandbox (§6.8) | Two executors behind `ProcessExecutor`. **Docker** for gate jobs **and opencode servers**: read-only rootfs and worktree, writable role mounts, noexec tmpfs, hardening flags, explicit container cleanup on stop/timeout; **kernel-level egress blocking** via a Docker `--internal` network (containers have no route out; the orchestrator connects to the container IP directly) with the allow-listing CONNECT proxy as the only outbound path. **Daemon-free namespaces** for gate jobs (`sandbox_namespaces`): unprivileged user/mount/net/UTS/IPC/PID namespaces, private `/proc`, the same mount layout built with bind mounts and pivot_root, fresh netns, Landlock path confinement, seccomp deny-list, cgroup limits when delegated (fail-closed otherwise). Both escape-tested |
| Custom clang-tidy checks (`cf-swt-*`) | **Not built** (needs LLVM 17+, see §9) |

Test suite: **547 tests**, green on `debug`, `release`, `asan` and `tsan`.

---

## 2. Architecture

The design's module map and the implemented directories:

| Module | Directory | Design § | Responsibility |
|---|---|---|---|
| cf-common | `include/cf/common`, `src/common` | 4.1 | `cf::Result`/`Status` (tl::expected), SHA-256 + idempotency keys, no-shell process executor |
| cf-store | `include/cf/store`, `src/store` | 6.12, 8.4 | SQLite (WAL) run store: runs, transitions, milestones, operations, run lease, locks, checkpoints, LLM ledger, gate results, defects |
| cf-orch | `include/cf/orch`, `src/orch` | 6.1, 6.12–6.13, 7, 12 | Dispatcher (intent-and-reconcile), run/milestone state machines, dependency-aware scheduler, review-validity policy, milestone loop, run orchestrator, escalation bundles, crash recovery, workspace driver |
| cf-llm | `include/cf/llm`, `src/llm` | 6.6 | OpenAI-compatible client (chat + embeddings) for Fireworks / local vLLM: retries with backoff, `Retry-After`, per-role concurrency, usage/cost accounting |
| cf-plan | `include/cf/plan`, `src/plan` | 6.2, 8.1 | Implementation plan parser (YAML or Markdown front matter), schema + DAG validation, FR-18 toolchain check |
| cf-eval | `include/cf/eval`, `src/eval` | 17.2, 18.2 | Golden evaluation suite harness: stratified manifest, outcome records, scoring with confidence intervals, held-out split, target checks, JSON/Markdown reports; hidden acceptance tests; Phase 0b model benchmark (`cf-eval bench`) that drives candidates through the real run pipeline |
| cf-gates | `include/cf/gates`, `src/gates` | 6.7 | Toolchain profiles (YAML), staged gate runner (prerequisites → cheap → expensive), gate plan builder, trusted step expansion |
| cf-rag | `include/cf/rag`, `src/rag` | 6.3 | Tokenizer, structure-aware chunking, BM25, hnswlib vector index (brute-force fallback behind the same interface), embeddings, front-matter loader, HTML cleaner, injection scanner, hybrid retriever with applicability and precedence, labeled retrieval evaluation (recall@k, MRR) |
| cf-refmirror | `include/cf/refmirror`, `src/refmirror` | 6.3.1 | Approved-documentation manifest, fetch + checksum + clean + scan + publish pipeline |
| cf-git | `include/cf/git`, `src/git` | 6.8.3, 10.4.5 | Worktrees, snapshot commits with CF trailers, diffs, file hashes, trailer recovery, squash merges |
| cf-sandbox | `include/cf/sandbox`, `src/sandbox` | 6.8 | Sandbox executors behind `ProcessExecutor`: Docker (gate jobs + opencode servers, internal-network egress) and daemon-free namespaces (user/mount/net/UTS/IPC/PID namespaces, Landlock, seccomp, cgroups) |
| cf-github | `include/cf/github`, `src/github` | 6.10, 10.4 | REST client (PRs, issues, comments, `cf-op` markers) and the pre-push checks |
| cf-mcp | `include/cf/mcp`, `src/mcp` | 6.4, 9.1 | MCP stdio server, role-filtered tool registry with JSON-schema validation, structured errors |
| cf-agents | `include/cf/agents`, `src/agents` | 6.6 | opencode adapter: HTTP client + SSE event stream (Phase 0a) |
| cf-cli | `include/cf/cli`, `src/cli` | 9.2 | `cf` commands |
| harness spike | `tests/harness_compat` | 6.6.2 | H1–H15 compatibility suite used to pin/upgrade opencode |

The orchestrator is a deterministic state machine (design D6). External effects
(opencode turns, commits, pushes, PRs) follow the intent-and-reconcile protocol: a
durable intent is committed before the effect, every attempt is stamped with the run
lease epoch, and recovery reconciles whatever was left in flight.

### Daemon-free sandbox algorithm (design §6.8)

`cf::sandbox::NamespaceExecutor` runs one command per invocation with no Docker and
no root. The full sequence:

1. **Pre-flight (parent).** If `memory_mb`/`pids_limit` are set, create a child
   cgroup under the delegated cgroup v2 subtree and write `memory.max`/`pids.max`;
   when delegation is missing the run fails closed instead of running unbounded.
   Open the stdout/stderr capture pipes, the close-on-exec error pipe (reports
   setup/exec failures) and a `socketpair` for the user-namespace handshake; pick
   the sandbox root `/tmp/cf-ns-<pid>-<n>`.
2. **User namespace.** The intermediate child calls `setpgid(0, 0)` (so a timeout
   can kill the whole tree), redirects stdout/stderr, then `unshare(CLONE_NEWUSER)`
   and announces on the socketpair. The parent writes `setgroups=deny`,
   `uid_map`/`gid_map` as `0 <invoking-uid> 1` and acknowledges; the child switches
   to the mapped root with `setresgid`/`setresuid(0, 0, 0)` — so the command runs
   as root inside while every file it creates stays owned by the host user.
3. **Namespaces and PID 1.** `unshare` with the mount, network, UTS, IPC and PID
   namespace flags, then fork: the command becomes **PID 1** of the new pid
   namespace. The intermediate mirrors the command's exit status; its exit tears
   the namespace down, so nothing can outlive a timed-out run. The fresh network
   namespace has only loopback — egress is kernel-blocked without a proxy.
4. **Mount layout (command).** `no_new_privs`, then bind mounts + `pivot_root`:

   | Target | Source / mode |
   |---|---|
   | `/usr`, `/usr/local`, `/bin`, `/sbin`, `/lib`, `/lib64`, `/etc` | host, read-only |
   | `/work` | worktree, read-only |
   | `/work/<writable>` | worktree/<writable>, read-write (missing dirs created first) |
   | `/tmp`, `$HOME` (`/home/cf`) | tmpfs, `noexec`, 64 MiB |
   | `/build` | tmpfs, executable (size from the spec) |
   | `/dev` | tmpfs + bound `null`/`zero`/`full`/`random`/`urandom` |
   | `/proc` | fresh procfs (private to the pid namespace) |

   `extra_readonly_mounts` are layered read-only; host `/home`, `/root`, `/var`,
   `/opt`, `/srv` stay invisible.
5. **Landlock.** Probe the ABI (`landlock_create_ruleset(NULL, 0, VERSION)`); below
   v1 the run fails closed. Rules attach **per mounted subtree** — bind mounts keep
   their original object hierarchy, so one rule on `/` would not cover them —
   read+execute on `/`, `/usr`, `/usr/local`, `/bin`, `/sbin`, `/lib`, `/lib64`,
   `/etc`, `/proc`, `/work` (and the extra read-only targets); full access only on
   `/dev`, `/build`, `/tmp`, `$HOME` and the writable paths. Handled access is
   ABI-gated (`REFER` ≥ v2, `TRUNCATE` ≥ v3). Missing paths are skipped; any other
   error fails closed. Rules survive `execve` and are inherited by children.
6. **seccomp.** A BPF filter checks `seccomp_data.arch` first (mismatch →
   `KILL_PROCESS`), then denies `ptrace`, `process_vm_readv/writev`, `bpf`,
   `userfaultfd`, `perf_event_open`, `keyctl`/`add_key`/`request_key`, `io_uring_*`,
   `mount`/`umount2`/`pivot_root`/`chroot`, `unshare`/`setns`,
   `open_by_handle_at`, `kcmp`, `pidfd_getfd`, kexec/module calls, `reboot`,
   swap, `acct`, time syscalls and `sethostname`/`setdomainname` with `EPERM`;
   everything else is allowed. Applied last, after all setup syscalls.
7. **Run and supervise.** The command `chdir`s to the mapped working directory and
   `execvp`s. The parent captures output until EOF or the deadline, then `SIGKILL`s
   the process group, reads the setup/exec errno from the error pipe, reaps,
   removes the cgroup (`cgroup.kill` + rmdir) and deletes the sandbox root tree.

Failures are fail-closed: any setup step (maps, mounts, Landlock, seccomp, cgroup)
reports its errno through the error pipe and the run is marked not launched. The
§6.8.4 escape suite covers each property, including probes that fail if a layer is
disabled.

---

## 3. Prerequisites

- Linux x86_64, Ubuntu 22.04+ (kernel 5.15+)
- CMake ≥ 3.25, Ninja, `git`
- GCC 11+ or Clang 17+ (framework builds as strict C++17)
- [vcpkg](https://github.com/microsoft/vcpkg) at the pinned baseline
  (`e456309491fd875487bf02b8a0dca76c1b1b00cc`, see `vcpkg.json`)
- Optional: Docker (for the future sandbox executor), opencode 1.18.35 +
  `FIREWORKS_API_KEY` (for the harness suite and real agent turns)

Third-party pins live in [`third_party/VERSIONS`](third_party/VERSIONS): opencode
1.18.35, the default model IDs, LLVM 17, GCC/Clang minimums.

---

## 4. Build

```bash
# one-time: vendor vcpkg at the pinned baseline (git-ignored)
git clone https://github.com/microsoft/vcpkg third_party/vcpkg
git -C third_party/vcpkg checkout e456309491fd875487bf02b8a0dca76c1b1b00cc
third_party/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# configure + build (vcpkg installs the manifest dependencies on first configure)
cmake --preset debug
cmake --build build/debug -j"$(nproc)"

# test
ctest --test-dir build/debug --output-on-failure
```

Presets (`CMakePresets.json`): `debug`, `release`, `asan` (ASan+UBSan),
`tsan` (ThreadSanitizer), `coverage`. The build applies
`-Wall -Wextra -Wpedantic -Werror` to framework code only.

**TSan note:** TSan needs low ASLR entropy on some kernels. Either
`sudo sysctl -w vm.mmap_rnd_bits=28` or run `setarch $(uname -m) -R ctest --test-dir build/tsan`.
GCC's libtsan before GCC 13 does not intercept `pthread_cond_clockwait`, so the
two cross-thread idle-tracker tests skip under that runtime (Clang TSan and
GCC 13+ run them); the namespace-sandbox tests skip under any TSan runtime, as
noted in §9; the large hnswlib parity test skips under ASan and TSan because
hnswlib's prefetch over-read and per-element lock order trip those runtimes
and GCC 11's cannot suppress them (the small HNSW tests still run).

**Binaries** land in `build/<preset>/`:

| Binary | Purpose |
|---|---|
| `cf` | CLI: `check`, `run`, `status`, `resume`, `escalations`, `report` |
| `cf-mcp` | MCP tool server over stdio (one per agent role) |
| `cf-mcp-spike` | Minimal MCP stub used by the Phase 0a harness |
| `cf-refmirror` | Reference documentation mirroring job |
| `cf-eval` | Golden suite validation/scoring, RAG evaluation, Phase 0b model benchmark |
| `cf-harness-spike` | opencode compatibility suite (H1–H15) |

---

## 5. Using the CLI

The workspace layout created by the CLI:

```
<workspace>/
  runs/<run_id>/run.db          # SQLite run store (WAL)
  runs/<run_id>/plan.yaml       # copy of the validated plan
  runs/<run_id>/escalations/    # escalation bundles
  wt/<run>/<milestone>/<role>/  # agent worktrees (driver)
  wt/<run>/integration/         # integration branch worktree
  build/<run>/<milestone>/      # out-of-source builds
  reports/<run>/<milestone>/    # gate reports
```

```bash
# validate a plan against the toolchain profiles (FR-18)
cf check --plan plans/product-x.yaml --profiles profiles

# ingest company documents: validates front matter, holds anything that looks
# like an injected instruction, writes normalized copies for cf-mcp --docs and
# records document versions (design sections 6.3.1, 8.4)
cf ingest --docs docs/ --out cf-work/docs --db cf-work/runs/R12/run.db

# create a run (validates, creates the store, registers milestones)
cf run --plan plans/product-x.yaml --profiles profiles --workspace cf-work --run-id R12

# execute it. Without --skip-refinement the design refiner runs first: the
# planner model writes runs/R12/design.{json,md} (component design, refined
# milestone DAG with write sets and interfaces, ADRs) and the run stops at the
# human approval gate. --skip-refinement bypasses the gate entirely.
cf run --plan plans/product-x.yaml --profiles profiles --workspace cf-work --run-id R12 \
       --execute --repo /path/to/target-repo \
       [--design-model accounts/fireworks/models/<planner>] \
       [--model coder=accounts/fireworks/models/<model>] \
       [--sandbox-image cf-toolchain-c-cpp:2026.10] [--docs cf-work/docs]

# review the design package, then approve it (AwaitingDesignApproval -> Executing)
cf approve --run R12 --workspace cf-work --stage design

# execute again: the run already exists (re-running is idempotent), the
# approved refined plan drives the scheduler (write sets, interfaces) and the
# pipeline runs to Done
cf run --plan plans/product-x.yaml --profiles profiles --workspace cf-work --run-id R12 \
       --execute --repo /path/to/target-repo

# inspect it
cf status --run R12 --workspace cf-work          # human-readable
cf status --run R12 --workspace cf-work --json   # machine-readable

# recover after a crash (lease takeover, stale locks, in-flight ops,
# worktree reset when --repo is given)
cf resume --run R12 --workspace cf-work --repo /path/to/target-repo

# escalation bundles
cf escalations --run R12 --workspace cf-work

# final report: report.json + report.md (traceability matrix, gates, defects, cost)
cf report --run R12 --workspace cf-work --out reports/R12
```

### Plans

A plan is YAML, or Markdown with YAML front matter (design §8.1):

```yaml
plan_id: PLAN-MULTICODER-X
product: Y
version: "1.0"
requirements_ref: repo://docs/requirements/Y-SRS.md@v3.2
milestones:
  - id: M1
    title: Core message bus
    requirements: [REQ-001, REQ-002]
    depends_on: []
    acceptance: ["Publish/subscribe latency p99 < 2 ms in-process"]
target:                # decides the generated code's language (design §4.2)
  language: cpp
  standard: c++20
  build_system: cmake
  test_framework: gtest
inference: fireworks   # or local-vllm
budget:
  max_usd_total: 250
```

Refined plans may additionally declare per milestone: `writes` (components/paths),
`uses` (public interfaces), `writes_interfaces` (interfaces the milestone changes) —
these drive the scheduler's parallel-safety rules (design §6.13.2).

### Toolchain profiles

`profiles/*.yaml` (design §6.7.3) contain **trusted commands only**: lists of steps,
each an argument list, executed with no shell. `{src_dir}`, `{build_dir}`,
`{standard}`, `{standard_number}`, `{standard_var}` and `{changed_files...}`
placeholders are expanded per element. `profiles/c-cpp.yaml` is provided; the
Python, TypeScript/JavaScript, Java and Go profiles follow the tools table in
design §6.7.2.

---

## 6. MCP tool server

`cf-mcp` implements the design's tool surface (§9.1) over MCP stdio, filtered by the
agent role:

```bash
cf-mcp --role coder --run R12 --milestone M3 \
       --db cf-work/runs/R12/run.db \
       --docs cf-work/docs
```

| Tool | Roles | Notes |
|---|---|---|
| `search_guidelines`, `get_document`, `get_requirement`, `get_design`, `lookup_reference` | all | RAG-backed; hits carry citations, precedence conflicts and `unresolved_conflict` flags; `lookup_reference` returns `covered:false` for mirror gaps |
| `get_pattern` | coder, reviewer | Pattern catalog entries |
| `get_gate_results`, `list_defects` | all | Read from the run store |
| `file_defect`, `verify_defect` | qa | Fingerprint de-duplication; recurring defects are marked `REOPENED` |
| `submit_work` / `submit_review` / `submit_tests` | coder / reviewer / qa | Structured turn endings |
| `dispute_test` | coder | Records a `DISPUTED` defect against a protected test and ends the turn; QA decides immediately (fix or reject), a second dispute of the same test escalates (design §6.9, §12) |
| `get_context`, `find_symbol`, `get_callers`, `run_checks` | – | Return `TOOL_UNAVAILABLE` until cf-codeintel/gates/sandbox land |

Arguments are validated against JSON schemas; failures return structured errors
(`TOOL_INVALID_ARGUMENTS`, `TOOL_DENIED`, `TOOL_NOT_FOUND`, `TOOL_UNAVAILABLE`).
Every `tools/call` is appended to `CF_MCP_CALLS_FILE` when set, so the orchestrator
can observe submissions.

---

## 7. Reference documentation mirror

`cf-refmirror` implements §6.3.1: only the URLs in `reference-docs/manifest.yaml`
are contacted, checksums are verified, HTML is cleaned to Markdown, content is
scanned for injected instructions, and the result is written as front-matter
Markdown (loadable by `cf-mcp --docs`) plus a raw audit snapshot and `report.json`.

```bash
cf-refmirror --manifest reference-docs/manifest.yaml --out mirror/ [--only boost]
```

Sets that fail the checksum or the injection scan are held and reported, never
indexed. Sources can be HTTP(S) or local files (useful for tests and air-gapped
setups).

---

## 8. Testing

```bash
ctest --test-dir build/debug --output-on-failure     # 547 unit tests
cmake --preset asan && cmake --build build/asan -j"$(nproc)" && ctest --test-dir build/asan
cmake --preset tsan && cmake --build build/tsan -j"$(nproc)" && setarch "$(uname -m)" -R ctest --test-dir build/tsan
```

Conventions: each module has a public interface header, its own CMake target and
unit tests; tests use temp directories, injected fakes (HTTP transports, agent
runners, recovery hooks) and real git repositories where behaviour depends on git.

**Harness compatibility suite** (Phase 0a, design §6.6.2) — required before any
opencode version is pinned or upgraded:

```bash
./build/debug/cf-harness-spike \
  --mcp "$PWD/build/debug/cf-mcp-spike" \
  --runs 20 --timeout 240
```

It needs the pinned opencode binary and a model provider key; results are written
to `spike-out/report-<timestamp>.json`.

### Golden evaluation suite (`cf-eval`, design §17.2)

The stratified suite lives in `eval/`: a manifest (`eval/suite.yaml`) listing
functional plans per language and tier plus reliability, security, governance and
conflict scenarios, each with a plan file whose hidden acceptance tests stay with
the harness. `cf-eval` validates the suite and turns recorded run outcomes into a
scored report:

```bash
cf-eval validate --suite eval/suite.yaml
cf-eval report --suite eval/suite.yaml --outcomes outcomes.json --out reports/eval --strict
```

The report scores milestone and run completion, hidden-test pass rate,
iterations, cost per milestone, escalation rate and reasons, security and
recovery pass rates — per plan, per stratum (category, language, tier), overall
and split into tuning vs held-out — with mean ± 1.96·SE intervals over the pooled
per-repeat samples, and checks the G1 completion target (functional plans) and
the 100% security and recovery targets (`--strict` exits non-zero on a miss).
Execution is driven through an injected `PlanExecutor`, so the harness is fully
testable offline.

#### Model benchmark (Phase 0b, design §18.2)

`cf-eval bench` runs each candidate model configuration in
`eval/benchmarks/*.yaml` (role → exact model id, provider URL, plans, repeats,
optional prices) through the **real run pipeline**: a fresh scratch repository,
a run store, real opencode servers, gates and integration. Afterwards it compiles
and runs the plan's hidden acceptance tests (`eval/hidden/<plan_id>/spec.yaml` —
argv steps executed with no shell, per-case counts taken from a final
`{"tests": N, "passed": M}` line) against the integration worktree. Outcomes are
read back from the run store, cost from the per-role `usage-<role>.jsonl` files,
and the winner is recommended by run completion, milestone completion,
hidden-test pass rate and lower cost:

```bash
cf-eval bench --suite eval/suite.yaml \
              --benchmark eval/benchmarks/fireworks.yaml \
              --workspace bench-work --out bench-out \
              --mcp build/debug/cf-mcp --profiles eval/profiles \
              [--candidate fireworks-default] [--plan t1-cpp-ring-buffer] \
              [--dry-run] [--drop-scratch]
```

`eval/profiles/` is the bare-host variant of the toolchain profiles: only
build and test steps, because the full `profiles/c-cpp.yaml` and
`profiles/python.yaml` format, guardrail and analysis steps need the
`cf-toolchain-c-cpp` / `cf-toolchain-python` sandbox images. Use the full
profiles when the bench runs against those images.

Each candidate writes `<name>-outcomes.json` and `<name>-report.{json,md}` into
`--out`, plus the combined `benchmark.{json,md}`; the run's scratch tree (plan
repositories, run stores, worktrees) is kept under `--workspace` unless
`--drop-scratch` is given. It needs `FIREWORKS_API_KEY` and the pinned opencode
binary. Plans with TSan tests need ASLR entropy lowered: run the bench under
`setarch $(uname -m) -R` (or apply the `vm.mmap_rnd_bits` sysctl from §4).

**RAG evaluation sets** (design §6.3) live in `eval/rag/`: a collection, a
cutoff `k`, a docs directory and queries with known relevant chunks
(`relevant`, exact chunk ids) or documents (`relevant_docs`), carrying the same
applicability context as a real run (`product`, `component`, `language`,
`as_of`) — so the filter is exercised, not bypassed:

```bash
cf-eval rag --set eval/rag/company.yaml            # recall@8 >= 0.85, MRR >= 0.6
cf-eval rag --set eval/rag/company.yaml --list-chunks   # labeling aid
```

`--warn-only` reports without failing; a miss exits non-zero for CI. The
starter set is small; the production set grows to the design's minimum of 200
labeled queries per collection.

The adversarial conflict set (`eval/rag/conflicts.yaml`) scores conflict
expectations instead of relevance: a resolved conflict must return the winner
with the overridden chunk named, and an unresolved requirement/policy
conflict must be flagged for escalation. Its queries carry no relevance
labels, so recall/MRR do not apply; every expectation must be met.

**CI** (`.github/workflows/ci.yml`): GCC 13 and Clang 18 × debug/release, plus
ASan/TSan jobs; a manually dispatched harness job runs H1–H15 against a pinned
opencode version.

---

## 9. Known gaps and roadmap

- **opencode run process.** `RunProcess` composes a whole run: the production
  factory writes each role's `opencode.json`, spawns `opencode serve`, opens the
  event stream (permissions → broker; questions → the RAG-backed policy, which
  answers only from high-precedence documents and records everything else as an
  ambiguity; `session.idle` → turn completion, since polling `/session/status`
  cannot tell "between tool steps" from "turn finished"), and
  `SessionAgentRunner` resolves drivers per (role, worktree);
  prompts come from the versioned `prompts/<role>.md` templates with embedded
  defaults. `cf run --execute` drives it (with `cf approve` for the design gate);
  an end-to-end test drives a full milestone to `Done` against a fake opencode
  server, with real git snapshots, gates and integration. Remaining for full
  fidelity: running the opencode servers in the daemon-free sandbox and the
  real-model runs of the golden suite (Phase 0b).
- **Sandbox (design §6.8).** The Docker-based executor covers gate jobs
  (`cf-sandbox`) and, since this session, the opencode servers themselves
  (`agent_sandbox_image` on the run process): each server runs in a container
  with a read-only rootfs, the worktree at `/work` (read-only, with the role's
  writable directories layered in), the run directory at `/cf`, noexec tmpfs,
  dropped capabilities, `no-new-privileges`, resource limits and a published
  loopback port; the process force-removes its container on stop or timeout,
  so nothing lingers. **Egress is kernel-blocked**: the factory ensures an
  internal Docker network (`cf-sandbox` by default, created with
  `--internal`), containers join it and have no route out — a direct connect
  fails with `ENETUNREACH` even if a process ignores the proxy. The
  orchestrator reaches each server by the container's IP on that bridge (no
  published ports, which internal networks cannot provide), and the built-in
  allow-listing CONNECT proxy, bound to the bridge gateway, is the only
  outbound path for the model endpoint (`sandbox_allowed_hosts`). The §6.8.4
  escape suite covers it.
- **Daemon-free sandbox (design §6.8).** `cf-sandbox`'s namespace executor
  runs a command in unprivileged user, mount, network, UTS, IPC and PID
  namespaces with no Docker and no root, with **Landlock** path confinement,
  a **seccomp** deny-list and best-effort **cgroup v2** limits (fail-closed
  when requested without delegation). The step-by-step algorithm, mount
  layout and filter policies are in
  [§2](#daemon-free-sandbox-algorithm-design-68). Gates use it with
  `sandbox_namespaces` on the driver config. The same escape suite runs
  against it (plus probes for the pid namespace, Landlock, seccomp and the
  cgroup fail-closed path); under TSan the tests skip because the sanitizer
  runtime's threads make `unshare(CLONE_NEWUSER)` fail with `EINVAL` (kernel
  rule), not because of the executor. Remaining: running agent (opencode)
  servers in the namespace sandbox, which needs a routed egress path for the
  model endpoint.
- **Custom clang-tidy checks (`cf-swt-*`).** Needs LLVM 17+ (`llvm-17-dev`,
  `clang-tidy-17`); not available in this environment's default repos. Check
  fixtures per design §6.5.
- **GitHub operations.** `cf-github` provides the REST client (pull requests,
  issues, comments, hidden `cf-op` markers), the full §10.4.6 pre-push check
  set (branch policy, protected paths, size, binaries, required trailers,
  secret scanning with findings that never carry the secret, and dependency
  licence checks against a local licence database and allow-list) and
  `cf-orch` provides the dispatcher handlers (`push` with compare-and-swap,
  never over a moved remote; `pr_create`; `pr_update` incl. closing;
  `issue_create`; `comment`), each idempotent and its own reconciler, plus
  `GithubFlow`, which the run process calls after every step: Done milestones
  push to `cf/<run>/<m>` and `cf/<run>/integration`, a draft PR opens on the
  first milestone, later states update it (skipping no-op syncs), `Done` marks
  it ready, escalation pushes `cf/<run>/escalation/<at>` and comments the
  bundle link, and abort closes the PR. The GitHub App identity signs an RS256
  JWT in memory and exchanges it for short-lived installation tokens, cached
  and refreshed before expiry; the CLI loads the key from a file
  (`--github-key-file`) and wires everything through. Defects are exported to
  GitHub Issues too (`DefectExporter`, §6.10): one issue per defect labelled
  `cf-defect`, `severity:*`, `milestone:*` and `requirement:*`, with the hidden
  fingerprint marker so a recurring defect reuses the existing issue across
  runs; status changes sync (close on VERIFIED, reopen on REOPENED), a human
  closing the issue is pulled back as `WONT_FIX`, bodies are secret-scanned
  and restricted runs post only IDs and the report link. Marking the PR ready
  requests the CODEOWNERS reviewers (design §10.4.4): the handler reads
  `.github/CODEOWNERS`, `CODEOWNERS` or `docs/CODEOWNERS` from the repository
  and requests users and teams on the pull request, again on reconcile after a
  crash (requesting is idempotent); a missing file simply means no reviewers.
- **Model benchmark (Phase 0b).** `cf-eval bench` implements the benchmark: the
  candidates in `eval/benchmarks/fireworks.yaml` run through the real pipeline on
  the evaluation plans, with hidden acceptance tests and scored reports under
  `bench-out/`; it needs `FIREWORKS_API_KEY` and the pinned opencode. The full
  benchmark (3 candidates × 7 plans, C++ T1/T2/T3, security, Python, Go and
  crash recovery) completed: the pinned candidate finished 6 of 7 runs Done
  with every hidden test green (79/79) at $0.16 per milestone, the best
  completion of the three; the pins in `third_party/VERSIONS` carry the
  provenance note. The T3 plan escalated for all three candidates even though
  its hidden acceptance tests pass: the final product is correct, but the
  revalidation fix turns over the old QA tests do not complete. Reliability scenarios
  inject a crash: a hidden spec's `crash_after_milestone` (or
  `crash_after_seconds`) makes the bench SIGKILL the run process mid-run, run
  `cf resume` (lease takeover, stale locks, in-flight operations, worktree
  reset) and continue in a second process; `rel-orchestrator-crash` verifies
  the run still reaches Done with no duplicated commits or operations.
  TSan-involving plans need `setarch $(uname -m) -R` on hosts with high ASLR
  entropy (same note as §4). Remaining: grow the suite (more plans per tier,
  reliability and governance scenarios) and re-run the benchmark as it grows.
- **Reliability fault injection.** The driver records a checkpoint before
  every agent turn (design section 6.12.3), and the bench kills the run
  process at a milestone boundary, resumes it (lease takeover, stale locks,
  in-flight operations, worktree reset to the checkpoint) and continues;
  `rel-orchestrator-crash` verifies the run still reaches Done with no
  duplicated commits or operations. The full §6.12.5 matrix (a kill at
  every protocol step for every operation kind, SIGSTOP takeover with a live
  GitHub request, sandbox kills) is still to be built.
- **RAG production index.** hnswlib is the default vector index (brute force
  stays behind the same `VectorIndex` interface as the parity reference); FAISS
  (IVF-PQ for collections over 5M chunks) is still pending. PDF/DOCX ingestion
  is pending; the labeled retrieval evaluation set is started (`eval/rag/`)
  and grows to 200 queries per collection, and the adversarial conflict set is
  in place.
- **System-level validation loop.** `WorkspaceDriver::validate_system` delegates to
  the agent runner; the full QA system suite comes with the real runner.

---

## 10. Repository layout

```
CMakeLists.txt              # targets for every module and binary
CMakePresets.json           # debug, release, asan, tsan, coverage
vcpkg.json                  # manifest, pinned baseline
third_party/VERSIONS        # opencode, model and toolchain pins
Coder_Framework_Design_Document.md
docs/phase0a/               # harness go/no-go decision + API notes
include/cf/<module>/        # public interface headers (one directory per module)
src/<module>/               # implementations
tests/unit/                 # GoogleTest unit tests (547)
tests/harness_compat/       # Phase 0a compatibility spike (H1-H15)
eval/                       # golden suite manifest/plans, hidden acceptance tests,
                            # benchmark configs and RAG evaluation sets (cf-eval)
reference-docs/manifest.yaml
catalog/patterns/           # curated pattern catalog (YAML, design §6.2)
prompts/                    # versioned role prompt templates
profiles/                   # toolchain profiles (YAML)
scripts/                    # dev helpers (e.g. safe cleanup of leaked servers)
AGENTS.md                   # agent guardrails (process safety, conventions)
```

---

## 11. Contributing

- Follow the design document; when behaviour is unclear, the design section is the
  source of truth and the code comments cite it.
- Framework code is C++17 only (no C++20), `-Werror`, no exceptions across module
  boundaries: fallible functions return `cf::Result<T>` / `cf::Status`.
- New modules: public header under `include/cf/<module>/`, sources under
  `src/<module>/`, its own CMake target, unit tests in `tests/unit/`, and error
  codes added to that module's registry.
- Changes to pinned versions (`third_party/VERSIONS`) require re-running the
  compatibility suite for opencode and the golden evaluation suite for models.
- Never commit secrets. Provider keys come from the environment
  (`FIREWORKS_API_KEY`, `VLLM_API_KEY`).
