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
| Run store, side-effect protocol, crash recovery | Implemented and tested; startup recovery (§6.12.4) runs automatically at the start of `cf run --execute` (lease takeover, leftover-container removal, in-flight grace period, reconcile), and `cf resume` releases the lease it takes |
| State machines, scheduler, review rules, milestone loop, run orchestrator | Implemented and tested; refined plans' declared write sets are enforced at snapshot time (§6.13.2); generated dependency lockfiles and nested build/cache paths are treated as artifacts, and a merge that changes a public interface revalidates the Done milestones that use it against the new integration head (§6.13.3; a revalidation fix starts from that head, gates run in a separate build directory, and test-only gate failures are repaired by QA). Human-waiting states have configurable business-day deadlines (§12.1): `cf run --execute` and `cf approve` move an expired design approval or escalation to `Aborted` with `approval_timeout`/`escalation_timeout`, and the resolved deadlines are stored on the run so every command sees the same values |
| Toolchain profiles and staged gate runner | Implemented and tested: stages 1–2 plus profile-driven stage-3 coverage/mutation gates, trusted step expansion and `cf.yaml` threshold overrides. C/C++, Python, Go, TypeScript/JavaScript and Java profiles are provided (`profiles/`); the built-in checker reads coverage.py/llvm-cov JSON, Go cover profiles, LCOV and JaCoCo XML and enforces the configured thresholds; C/C++ and Java coverage ship instrumented scripts (CMake/Meson/Make llvm-cov and Maven/Gradle JaCoCo) and Python mutation runs through a mutmut wrapper, Go mutation through gremlins, Java mutation through PIT, TypeScript/JavaScript mutation through Stryker and C/C++ mutation through Mull, with Python's `mutmut-json` score enforced by the gate |
| RAG retrieval (BM25 + vectors, precedence, conflicts) and document ingestion | Implemented and tested; the `rag` config section selects hnswlib, optional FAISS HNSW/IVF-PQ or brute force, the hybrid BM25/vector weights, the default `top_k` (refiner, MCP search and role servers) and the document-type precedence. `models.embeddings` wires the OpenAI-compatible embeddings provider into the refiner, question answering and every role MCP server, so hybrid vector search is active when configured (BM25-only otherwise). `cf ingest` also converts PDFs with poppler and DOCX with Pandoc when a `<file>.meta.yaml` metadata sidecar is present |
| MCP tool surface (role-filtered, schema-validated) | Implemented and tested: RAG, reference lookup, code intelligence, defects, submissions, `run_checks` and `request_waiver` |
| Reference documentation mirror (`cf-refmirror`) | Implemented and tested: manifest validation, fetch, checksum, HTML→Markdown cleaning, injection scan, audit snapshot and report. `cf run` can now mirror the manifest into the run, pin the versions named by `vcpkg.json`/`requirements.txt`/`package.json`, index the result in the refiner, question answering and role MCP servers, and filter `lookup_reference` to the pinned version. Archive sources with `extract` patterns are unpacked with the system `tar`/`unzip`; generated Doxygen sets clone their approved repo/ref, run Doxygen and index the cleaned HTML. Remaining: the checked-in manifest still has placeholder URLs/checksums |
| Git layer (worktrees, trailers, diffs, squash merges) | Implemented and tested |
| Code intelligence (§6.5) | **Foundation + run integration**: the back-end interface and registry, the SQLite symbol index (incremental per file), tree-sitter back ends with the vendored **C, C++, Python, Go, TypeScript/JavaScript and Java** grammars, and a **Clang LibTooling** back end for C/C++ when LLVM/Clang 17 is available (symbols, methods qualified `Class.method`/`Receiver.Method`, call edges with caller attribution, imports/includes, doc comments and docstrings, the layering check; tree-sitter remains the fallback), the context packer (seeds → dependencies → callers → file siblings, cut to the token budget) and the `find_symbol`/`get_callers`/`get_context` MCP tools reading the index (`cf-mcp --codeintel-db`). When a back end matches the plan's language, the run indexes the integration worktree at start-up and refreshes the index incrementally at every merge (`CodeIntelSync`), and the role MCP servers are pointed at it. `get_context` also appends design/architecture hits (ADRs and contracts) within the remaining token budget. Remaining: the rest of the company guardrail rule set |
| Workspace milestone driver (agents + git + gates) | Implemented and tested; fix turns receive the milestone's open defects and every fix turn counts one attempt per defect (escalates at 4, §12); test authoring commits language-specific test paths (`tests/`, `src/test/`, `*_test.go`, `*.test.ts`, …), and each review resets the reviewer worktree to the coder's current HEAD |
| opencode-backed `AgentRunner` | Implemented end to end: turn driving with event-driven (`session.idle`) turn completion, permission broker, event routing, role configs, versioned prompt templates, RAG-backed question answering, server spawning, dead-server detection and restart before the next turn (§6.12, H8), UTF-8 sanitization of untrusted prompt/tool output before JSON and HTTP, `RunProcess` and `cf run --execute` |
| GitHub operations | Complete lifecycle: client (PRs, issues, comments, `cf-op` + fingerprint markers), pre-push checks (branch policy, protected paths, size, binaries, trailers, signatures, base freshness, secret scanning, dependency licences) that run on the exact commit before every flow push — a rejection records no intent and escalates the run, dispatcher handlers (`push`, `pr_create`, `pr_update` incl. close, `issue_create` with fingerprint de-dupe, `issue_update`, `comment` with late-landing duplicate deletion, §6.12.6 rule 4), `GithubFlow` (milestone pushes, PR open/update/ready with `pr.draft_until_done`/labels/`request_codeowners` policy and a report-derived body — milestone/gate summary, requirement traceability, defects, waivers and cost, escalation branch + comment, abort closes the PR), defect export to GitHub Issues (`DefectExporter`: one issue per defect with labels, status sync, human closure → WONT_FIX, secret-scanning and restricted-run redaction), GitHub App auth (RS256 JWT → short-lived installation tokens), SSH commit signing, branch-retention cleanup (`cf cleanup`), base-move rebase/re-gate, generated ruleset payloads (`cf rulesets`) and CLI wiring. Marking the PR ready requests the CODEOWNERS reviewers (`@user` → reviewer, `@org/team` → team reviewer; GitHub's `.github/`, root and `docs/` lookup order; absent file is fine; repeated on reconcile, which is idempotent) |
| CLI (`cf check/run/status/resume/escalations/report/approve/abort/ingest/cleanup/remind/replay/rulesets`) | Implemented and tested |
| Configuration file (§15) | `cf run --config` (default `./cf.yaml`): strict schema, flags > file > defaults, plan budget wins over file defaults; provider/role-model resolution with no provider mixing (§13.1); budget (cost, tokens, iterations, defect attempts and the active wall-clock limit, which excludes states waiting for people, §12), design-approval, escalation and waiver deadlines in business days (§12.1), GitHub push scheduling (`push_on`) and commit signing (`signing_key_ref`), pre-push policy (`branch_prefix`, `protected_paths`, `max_file_mb`) and PR policy (`draft_until_done`, `labels`, `request_codeowners`), RAG policy (`index`, `top_k`, hybrid weights, precedence), reference-docs manifest/version policy, branch retention, coverage/mutation overrides, loop/review-threshold/gate-resource knobs wired |
| Design refinement (Phase 4) | Implemented and verified with the real planner model: the refiner turns the plan into a design package (`runs/<run>/design.{json,md}` — component design, refined milestone DAG with write sets/uses/interfaces for the scheduler, ADRs), self-critiques and revises once, stops at the human approval gate, and the approved package drives execution. A reviewer can request changes with written feedback, which is archived and fed into the next refinement. Approving publishes the design as a product-scoped RAG design document (`<workspace>/designs/`, status `approved`; an earlier design for the same plan becomes `superseded` and is no longer retrieved), and the run's MCP servers and question policy carry the product/language applicability context. The curated pattern catalog (`catalog/patterns/`, six entries) is loaded by `cf-mcp --patterns`, is authoritative for `get_pattern` and is indexed into retrieval; the refiner prompt carries it, every ADR must name a catalog `pattern_id`, and refinement rejects a design that misses or invents one |
| Observability (§6.11, §14) | `cf-telemetry` emits one JSON object per log line with `run_id`, `milestone_id`, `agent` and `trace_id`; `cf`/`cf-mcp`/`cf-eval`/`cf-refmirror` install it, and the run process plus driver scopes each turn and gate run. The report and PR body aggregate `usage-<role>.jsonl` into cost/tokens per role and milestone, and `report.json` carries the design's metric samples (gate pass ratio, iterations, open defects, LLM tokens/USD, escalations, RAG latency) and exports it as OTLP/HTTP JSON when `OTEL_EXPORTER_OTLP_ENDPOINT` is set. Direct `cf-llm` calls and opencode HTTP/SSE agent traffic are archived gzip-compressed and AES-256-GCM encrypted under `runs/<run>/llm` and `runs/<run>/opencode` (`CF_ARCHIVE_KEY`, 64 hex characters); `cf replay` re-sends direct LLM calls. |
| Golden evaluation suite (`cf-eval`) | Implemented and tested: suite validation, scoring with confidence intervals and target checks, RAG retrieval evaluation, hidden acceptance tests, and the Phase 0b model benchmark (`cf-eval bench`) that runs candidates through the real run pipeline and ranks them; the starter suite now has 26 plans spanning C++, Python, Go, Java and TypeScript, including cross-component C++, Go, Java, Python and TypeScript pipelines, plus crash-injection reliability and security scenarios; the original 3-candidate × 7-plan benchmark confirms the pinned models; the expanded 19-plan runs are complete for all three candidates. The pinned candidate is best: **18/19 runs Done**, **0.9474 milestone completion**, **0.8816 hidden-test pass**, and **$0.1490/milestone**; `qa-glm-flash` reached 15/19 and `reviewer-kimi-fast` 16/19. The five remaining failure classes (T3 C++ revalidation, T3 Java compile, T2/T3 TypeScript write-set/test setup, and Python redaction) have since been fixed at the plan/seed level and individually rerun green |
| Sandbox (§6.8) | Two executors behind `ProcessExecutor`. **Docker** for gate jobs **and opencode servers**: read-only rootfs and worktree, writable role mounts (including the plan's root write-set files and build directories), docs/reference/design mounts, noexec tmpfs, hardening flags, explicit container cleanup on stop/timeout plus run-labelled cleanup on recovery; **kernel-level egress blocking** via a Docker `--internal` network (containers have no route out; the orchestrator connects to the container IP directly) with the allow-listing CONNECT proxy as the only outbound path. **Daemon-free namespaces** for gate jobs (`sandbox_namespaces`) **and opencode servers** (`agent_sandbox_namespaces`): unprivileged user/mount/net/UTS/IPC/PID namespaces, private `/proc`, the same mount layout built with bind mounts and pivot_root, fresh netns with loopback, Landlock path confinement, seccomp deny-list, cgroup limits when delegated (fail-closed otherwise), and `cf-ns-agent` bridging the host API port and the allow-listing egress proxy over Unix sockets. The Docker and gate-namespace executors run the escape suite; the namespace agent bridge has end-to-end coverage |
| Company guardrail checks (`cf-swt-*` / `swt.*`) | **Foundation**: 18 `cf-tidy` checks plus 32 Semgrep rules. `cf-tidy` runs `cf-swt-no-raw-new`, `cf-swt-no-raw-delete`, `cf-swt-no-raw-malloc`, `cf-swt-no-detached-thread` (CG-4.2), `cf-swt-no-hardcoded-secret`, `cf-swt-no-bool-param`, `cf-swt-no-c-style-cast`, `cf-swt-no-using-namespace-in-header`, `cf-swt-no-goto`, `cf-swt-no-reinterpret-cast`, `cf-swt-no-const-cast`, `cf-swt-no-raw-array`, `cf-swt-no-unsafe-c-function`, `cf-swt-no-multiple-inheritance`, `cf-swt-no-public-data-member`, `cf-swt-no-public-static-data`, `cf-swt-no-sleep-in-test` and `cf-swt-no-weak-hash` over the target's compilation database, with fixture tests for placement new, exempt namespaces, C allocation, macro use sites and clean sources. The C/C++ profile runs the standard clang-tidy check set plus `cf-tidy`. The Python, TypeScript/JavaScript, Java and Go profiles load starter `swt.*` Semgrep rules for shell/eval/HTML injection, unsafe YAML/TLS, stack traces, hardcoded secrets, unsafe eval/exec/os.system/pickle, disabled TLS verification, document.write, weak hashes, Java object deserialization and sleeps in tests. A starter rule-coverage catalog lives in `rules/guardrails.yaml`, and `cf-tidy --list-checks` lists the compiled checks. Remaining: the rest of the guideline rule set and optional plugin packaging for shared-LLVM distributions |

Test suite: **784 tests**, green on `debug`; CI also runs the `release`, ASan and TSan presets.

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
| cf-rag | `include/cf/rag`, `src/rag` | 6.3 | Tokenizer, structure-aware chunking, BM25, hnswlib vector index, optional FAISS HNSW/IVF-PQ backends (brute-force fallback behind the same interface), embeddings, front-matter loader, HTML cleaner, injection scanner, hybrid retriever with applicability and precedence, labeled retrieval evaluation (recall@k, MRR) |
| cf-refmirror | `include/cf/refmirror`, `src/refmirror` | 6.3.1 | Approved-documentation manifest, fetch + checksum + clean + scan + publish pipeline |
| cf-git | `include/cf/git`, `src/git` | 6.8.3, 10.4.5 | Worktrees, snapshot commits with CF trailers, diffs, file hashes, trailer recovery, squash merges |
| cf-sandbox | `include/cf/sandbox`, `src/sandbox` | 6.8 | Sandbox executors behind `ProcessExecutor`: Docker (gate jobs + opencode servers, internal-network egress) and daemon-free namespaces for gates and opencode servers, with the `cf-ns-agent` host/namespace bridge, Landlock, seccomp and cgroups |
| cf-github | `include/cf/github`, `src/github` | 6.10, 10.4 | REST client (PRs, issues, comments, branches, `cf-op` markers), pre-push checks and ruleset payloads |
| cf-mcp | `include/cf/mcp`, `src/mcp` | 6.4, 9.1 | MCP stdio server, role-filtered tool registry with JSON-schema validation, structured errors, agent-side gate dry run (`run_checks`) and waiver requests |
| cf-codeintel | `include/cf/codeintel`, `src/codeintel` | 6.5 | Language back-end interface + registry, SQLite symbol index (per-file incremental), Clang LibTooling C/C++ back end (optional LLVM 17) with tree-sitter fallback, tree-sitter back ends (vendored C, C++, Python, Go, TypeScript/JavaScript and Java grammars), context packer, architecture layering check |
| cf-tidy | `include/cf/tidy`, `src/tidy` | 6.5, 6.7.1 | Company C/C++ guardrail checks and the `cf-tidy` gate driver |
| cf-agents | `include/cf/agents`, `src/agents` | 6.6 | opencode adapter: HTTP client + SSE event stream (Phase 0a) |
| cf-cli | `include/cf/cli`, `src/cli` | 9.2 | `cf` commands |
| cf-telemetry | `include/cf/telemetry`, `src/telemetry` | 6.11, 14 | JSON log formatter and thread-local run/milestone/agent/trace context |
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
- Optional: Docker for the container sandbox (gate jobs and agent
  servers), opencode 1.18.35 + `FIREWORKS_API_KEY` (for the harness suite
  and real agent turns)

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

# optional: Clang LibTooling C/C++ code intelligence and cf-tidy (LLVM/Clang 17)
# cmake --preset debug -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm \
#   -DClang_DIR=/path/to/llvm/lib/cmake/clang
# Without these, cf-codeintel builds the tree-sitter C/C++ fallback and the
# cf-tidy company-check target is omitted.
# Optional FAISS RAG indexes:
# cmake --preset debug -DFAISS_ROOT=/path/to/faiss/prefix

# test
ctest --test-dir build/debug --output-on-failure
```

Presets (`CMakePresets.json`): `debug`, `release`, `asan` (ASan+UBSan),
`tsan` (ThreadSanitizer), `coverage`. The build applies
`-Wall -Wextra -Wpedantic -Werror` to framework code only.

**TSan note:** TSan needs low ASLR entropy on some kernels. Either
`sudo sysctl -w vm.mmap_rnd_bits=28` or run `setarch $(uname -m) -R ctest --test-dir build/tsan`.
Hidden benchmark steps can set `disable_aslr: true`, which makes the evaluator
wrap that step in `setarch -R`; this covers the hidden TSan programs but not
TSan builds the QA agent runs itself.
GCC's libtsan before GCC 13 does not intercept `pthread_cond_clockwait`, so the
two cross-thread idle-tracker tests skip under that runtime (Clang TSan and
GCC 13+ run them); the namespace-sandbox tests skip under any TSan runtime, as
noted in §9; the large hnswlib parity test skips under ASan and TSan because
hnswlib's prefetch over-read and per-element lock order trip those runtimes
and GCC 11's cannot suppress them (the small HNSW tests still run).

**Binaries** land in `build/<preset>/`:

| Binary | Purpose |
|---|---|
| `cf` | CLI: `check`, `run`, `status`, `resume`, `escalations`, `report`, `approve`, `abort`, `ingest` |
| `cf-mcp` | MCP tool server over stdio (one per agent role) |
| `cf-mcp-spike` | Minimal MCP stub used by the Phase 0a harness |
| `cf-refmirror` | Reference documentation mirroring job |
| `cf-tidy` | Company C/C++ guardrail checks (`cf-swt-*`) over a compilation database |
| `cf-eval` | Golden suite validation/scoring, RAG evaluation, Phase 0b model benchmark |
| `cf-ns-agent` | Host ↔ namespace bridge for sandboxed opencode servers |
| `cf-harness-spike` | opencode compatibility suite (H1–H15) |

---

## 5. Using the CLI

The workspace layout created by the CLI:

```
<workspace>/
  runs/<run_id>/run.db          # SQLite run store (WAL)
  runs/<run_id>/plan.yaml       # copy of the validated plan
  runs/<run_id>/design.{json,md}# approved design package (Phase 4)
  runs/<run_id>/codeintel.db    # symbol index (when a back end matches)
  runs/<run_id>/rules/          # copy of the company guardrail rules
  runs/<run_id>/opencode/       # encrypted opencode HTTP/SSE traffic archive
  runs/<run_id>/usage-<role>.jsonl  # per-turn agent cost/tokens (opencode)
  runs/<run_id>/escalations/    # escalation bundles
  designs/                      # published approved designs (RAG input)
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
       --execute --repo /path/to/target-repo [--config cf.yaml] \
       [--design-model accounts/fireworks/models/<planner>] \
       [--model coder=accounts/fireworks/models/<model>] \
       [--sandbox-image cf-toolchain-c-cpp:2026.10] [--docs cf-work/docs] \
       [--agent-sandbox-namespaces --ns-agent build/debug/cf-ns-agent] \
       [--ns-mount HOST:TARGET[:ro]] [--ns-path PREFIX] [--ns-env KEY=VALUE]

# review the design package, then approve it (AwaitingDesignApproval -> Executing)
cf approve --run R12 --workspace cf-work --stage design

# or send it back for revision with written feedback; the next execution
# refines again using that feedback (design section 7.1)
cf approve --run R12 --workspace cf-work --request-changes \
       --reason "Route construction through the registry factory"

# record a human waiver of a gate rule failure (design §10.2); agents cannot
# waive checks. The waiver and its approver appear in report.json/md and the PR.
cf approve --run R12 --workspace cf-work \
       --waiver guardrails --scope src/bus.cpp:42 \
       --justification "Legacy API kept for the compatibility window" \
       --approved-by alice [--milestone M3]

# refuse a pending agent waiver request instead
cf approve --run R12 --workspace cf-work --refuse-waiver wreq_1a2b3c4d5e6f7788 \
       --reason "fix the rule instead"

# execute again: the run already exists (re-running is idempotent), startup
# recovery reconciles whatever a killed orchestrator left behind (§6.12.4),
# the approved refined plan drives the scheduler (write sets, interfaces)
# and the pipeline runs to Done
cf run --plan plans/product-x.yaml --profiles profiles --workspace cf-work --run-id R12 \
       --execute --repo /path/to/target-repo

# inspect it
cf status --run R12 --workspace cf-work          # human-readable
cf status --run R12 --workspace cf-work --json   # machine-readable

# recover after a crash (lease takeover, stale locks, in-flight ops,
# worktree reset when --repo is given; force-removes containers left by the
# killed orchestrator and releases the lease afterwards)
cf resume --run R12 --workspace cf-work --repo /path/to/target-repo

# escalation bundles
cf escalations --run R12 --workspace cf-work

# a human aborts an escalated run; the designs it published are withdrawn
cf abort --run R12 --workspace cf-work --reason "superseded by R13"

# final report: report.json + report.md (traceability, gates, defects, cost,
# waivers, agent usage per role/milestone and the metrics snapshot)
cf report --run R12 --workspace cf-work --out reports/R12

# delete the run's cf/ branches once its PR has been merged/closed for
# github.branch_retention_days (idempotent; safe to run from a cron job)
cf cleanup --run R12 --workspace cf-work --repo /path/to/target-repo \
           [--config cf.yaml] [--retention-days 30]
# or sweep every run in the workspace (the usual cron invocation)
cf cleanup --all --workspace cf-work --repo /path/to/target-repo

# post due waiting-state reminders (design approval after 2/4 business days,
# escalation after 1/2, waiver requests after 1) on the run's PR; --all is the
# usual cron invocation
cf remind --run R12 --workspace cf-work [--config cf.yaml]
cf remind --all --workspace cf-work [--config cf.yaml]

# write the GitHub-side ruleset payloads; a human with admin rights applies
# them (the CF bot deliberately has no administration permission)
cf rulesets --out rulesets/ [--required-check "ci / build"] \
            [--required-check "ci / test"]

# re-send an archived direct LLM call (needs CF_ARCHIVE_KEY, the same key used
# when the call was archived) to the archived or an overridden model
cf replay --run R12 --workspace cf-work --call call_1a2b3c4d5e6f7788 \
          [--model accounts/fireworks/models/<model>]
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
  max_wall_clock_hours: 36   # active time; human-waiting states do not count
approvals:                   # business days; overrides cf.yaml defaults
  design_approval: 4d
  escalation: 2d
  waiver: 2d
```

Refined plans may additionally declare per milestone: `writes` (components/paths),
`uses` (public interfaces), `writes_interfaces` (interfaces the milestone changes) —
these drive the scheduler's parallel-safety rules (design §6.13.2).

### Toolchain profiles

`profiles/*.yaml` (design §6.7.3) contain **trusted commands only**: lists of steps,
each an argument list, executed with no shell. `{src_dir}`, `{build_dir}`,
`{standard}`, `{standard_number}`, `{standard_var}`, `{rules_dir}` and
`{changed_files...}` placeholders are expanded per element, including the stage-3
`{coverage_line}`/`{coverage_branch}`/`{mutation_min_score}` values. Coverage
steps can declare `format`/`report` for the built-in threshold checker
(`coverage-json`, `go-cover`, `lcov`, `llvm-cov-json` or `jacoco-xml`).
`profiles/c-cpp.yaml`, `profiles/python.yaml`, `profiles/go.yaml`,
`profiles/typescript.yaml` (TypeScript and JavaScript, npm/pnpm, vitest/jest)
and `profiles/java.yaml` (Maven/Gradle, JUnit 5) are provided. The profile
supplies the coverage/mutation tool metadata and optional stage-3 steps; the
Python, Go and TypeScript profiles ship line/branch coverage steps checked by
the built-in parser. The C/C++ profile runs `rules/scripts/cpp-coverage.sh` for
CMake, Meson and Make projects: it configures a separate Clang-instrumented
build, runs CTest, Meson test or `make test`, merges the profiles and exports
`llvm-cov-json`; `CF_COVERAGE_LDFLAGS` can append linker flags on hosts with a
non-default libstdc++ path. The Java profile runs `rules/scripts/java-coverage.sh`, which
invokes Maven or Gradle and normalizes the JaCoCo XML report. The Python
profile runs `rules/scripts/python-mutation.sh`, which copies the worktree,
runs mutmut in isolation, exports CI/CD stats and enforces the configured
score; the gate also reads the `mutmut-json` report and checks the minimum. The
Go profile runs `rules/scripts/go-mutation.sh`, which invokes gremlins and
enforces its efficacy score. The Java profile runs
`rules/scripts/java-mutation.sh`, which invokes PIT through Maven or Gradle and
checks the `mutations.xml` efficacy. The TypeScript/JavaScript profile runs
`rules/scripts/ts-mutation.sh`, which invokes Stryker with the project's test
runner and checks the JSON mutation score. The C/C++ profile runs
`rules/scripts/cpp-mutation.sh` (CMake, Meson or Make projects) with Mull and
checks each test target's mutation score. The C/C++ analysis stage runs Clang Static Analyzer through
`clang-tidy` (`clang-analyzer-*`) plus `cppcheck`.

### Company guardrails (`cf-tidy`)

When LLVM/Clang 17 is enabled, `cf-tidy` runs the company `cf-swt-*` checks over
the target's `compile_commands.json`. The first rules are
`cf-swt-no-raw-new`, `cf-swt-no-raw-delete`, `cf-swt-no-raw-malloc` and
`cf-swt-no-detached-thread` (CG-4.2) flag raw `new`, `delete`, C allocation/free
calls and detached threads in C++ project code while allowing placement new,
system headers and namespaces passed with `--allowed-namespaces=<regex>`.
`cf-swt-no-hardcoded-secret` flags credential-like variables initialized with a
string literal. `cf-swt-no-bool-param` flags boolean parameters that the
interface guideline asks authors to replace with a meaningful enum or split
operation. `cf-swt-no-c-style-cast` flags C-style casts in favour of
`static_cast` or a named conversion; `cf-swt-no-using-namespace-in-header`
prevents using-directives from leaking through headers, `cf-swt-no-goto`
asks for structured control flow in C++, and `cf-swt-no-reinterpret-cast` /
`cf-swt-no-const-cast` require typed conversions and const correctness;
`cf-swt-no-raw-array` asks for `std::array`/`std::vector` over C arrays in
C++; `cf-swt-no-unsafe-c-function` rejects unbounded string APIs such as
`strcpy` and `sprintf`; `cf-swt-no-multiple-inheritance` asks for composition
over inheritance; `cf-swt-no-public-data-member` and
`cf-swt-no-public-static-data` keep class data private, and
`cf-swt-no-sleep-in-test` enforces deterministic tests; `cf-swt-no-weak-hash`
rejects MD5/SHA-1 calls. Matching Semgrep rules cover the other languages.
`cf-tidy` also reads `cf-swt-*.AllowedNamespaces` from a
`.clang-tidy` file found by walking up from the compilation database, so the
same configuration can be used by standard clang-tidy and the company checks.
`cf-tidy --list-checks` lists the compiled checks; individual checks can be
disabled for a legacy component with `--disable-check=<id>`, or a focused run
can be limited with `--only-check=<id>`. `cf-tidy --coverage=<catalog>` reports
automated, reviewer-only and compiled-check counts from
`rules/guardrails.yaml`.

```bash
build/debug/cf-tidy -p build/debug --allowed-namespaces='^::allocator$' src/a.cpp
```

It exits non-zero when it finds a violation and prints clang-style diagnostics
tagged with the rule ID. `profiles/c-cpp.yaml` runs the standard `clang-tidy`
check set (with warnings treated as errors) and then `cf-tidy`, so a guardrail
failure blocks the gate. The pinned LLVM package is static, so `cf-tidy` is a
small clang-tidy-compatible driver rather than a loadable plugin; the remaining
rules and optional plugin packaging are tracked in §9.

### Non-C/C++ guardrails (`rules/semgrep`)

The non-C/C++ profiles load 32 starter company rules through Semgrep:
`python.yaml`, `typescript.yaml`, `javascript.yaml`, `java.yaml` and `go.yaml`.
`{rules_dir}` is resolved from `CF_RULES_DIR` when set, otherwise from the
resolved profile's repository (`<repo>/rules`), and is copied into the run
directory so both gate jobs and agent-side `run_checks` see the same trusted
files. Docker and namespace gate sandboxes mount it read-only at `/cf-rules`.
The rules cover high-signal hazards such as `shell=True`, unsafe YAML loading,
`eval`, untrusted `innerHTML`, `Runtime.exec`, `exec.Command("sh", "-c", ...)`,
disabled TLS verification, stack-trace printing and hardcoded secrets. Rule IDs
are immutable and use the `swt.<language>.<rule>` form; findings are gate
failures because the profiles pass `--error`.

```bash
semgrep --validate --config rules/semgrep/python.yaml
semgrep --config rules/semgrep/typescript.yaml --error src/frontend
```

Add one rule per mandatory guideline item as the company rule set grows. The
C/C++ checks stay in `cf-tidy`; see `rules/semgrep/README.md` for the rule
authoring contract.

### Configuration file (`cf.yaml`, design §15)

`cf run` loads `--config <path>`, defaulting to `./cf.yaml` when it exists.
Precedence is **flags > cf.yaml > built-in defaults**; a plan's `budget` and
`approvals` blocks still win over the file (design Q5, §12.1). The schema is
strict: an unknown key or a documented-but-unimplemented knob fails with
`CLI_CONFIG_INVALID` naming the key, so a configuration never silently
half-applies.

Supported sections and keys:

```yaml
run:
  workspace: /srv/cf/work       # run store, worktrees
  repo: /srv/checkouts/app      # a local checkout path (cloning is not implemented)
  base_branch: main

github:
  app_id: "12345"               # GitHub App identity; private_key_ref is a PEM path
  installation_id: "67890"
  private_key_ref: /srv/keys/github-app.pem   # secretmanager:// is rejected
  signing_key_ref: /srv/keys/cf-signing       # SSH key for signed commits
  branch_prefix: "cf/"          # pre-push branch policy
  protected_paths: [".github/**", "CODEOWNERS", "cf.yaml", "deploy/**"]
  max_file_mb: 5                # pre-push size limit
  push_on: [milestone_done, run_done, escalation]   # when CF pushes
  branch_retention_days: 30     # delete cf/ branches this long after close
  pr:
    draft_until_done: true      # false: open the PR ready immediately
    labels: [cf-generated]      # applied to the PR (added, never removed)
    request_codeowners: true    # ready-time reviewer request

providers:                      # OpenAI-compatible endpoints
  fireworks: { base_url: https://api.fireworks.ai/inference/v1, api_key_env: FIREWORKS_API_KEY }
  local-vllm: { base_url: http://vllm-host:8000/v1, api_key_env: VLLM_API_KEY }

models:
  default_provider: fireworks
  roles:
    planner:  { family: deepseek, fireworks: "<model-id>", temperature: 0.2 }
    coder:    { family: deepseek, fireworks: "<model-id>", temperature: 0.1, max_output_tokens: 8192 }
    reviewer: { family: kimi,     fireworks: "<model-id>", temperature: 0.0 }
    qa:       { family: glm,      fireworks: "<model-id>" }
  embeddings:                   # hybrid retrieval (design §6.3)
    fireworks: { model: "<embedding-model>", dim: 768, index: hosted }

approvals:                      # business-day deadlines (§12.1)
  design_approval: 5d
  escalation: 3d
  waiver: 2d

budget:                         # defaults only; a plan's budget wins
  max_usd_total: 150
  max_tokens_total: 60000000
  max_iterations_per_milestone: 12
  max_attempts_per_defect: 4
  max_wall_clock_hours: 24      # active time; Created/human-waiting excluded

rag:
  index: hnswlib                # hnswlib, faiss, faiss-ivfpq or brute-force
  top_k: 8                      # refiner context, MCP search default
  hybrid: { bm25_weight: 1.0, vector_weight: 1.0 }  # RRF contributions
  precedence: [policy, requirements, architecture, design, guideline, pattern, external_reference]
                                # listed first; missing types keep the built-in order

reference_docs:
  manifest: ./reference-docs/manifest.yaml
  index_version: pinned_at_run_start
  version_source: auto        # vcpkg.json / requirements.txt / package.json
  on_missing_version: report  # report (skip the set) or fail

gates:
  profiles_dir: ./profiles
  profile: auto        # or pin an id such as c-cpp; FR-18 still applies
  coverage: { line: 0.85, branch: 0.75, scope: changed }
  mutation: { enabled: true, min_score: 0.60 }
review:
  diff_threshold_lines: 400
  blocking_severities: [blocker, major]
sandbox:
  cpu: 8
  memory_gb: 16
  job_timeout_min: 30
  network: deny                 # gate jobs are always network-isolated
```

Behaviour worth knowing:

- **Providers/models.** Every agent role (coder, reviewer, qa) must resolve a
  model id for `default_provider`; a run never mixes providers (design §13.1).
  `planner` is optional and falls back to the coder model. Per-role
  `temperature` / `max_output_tokens` go into the opencode model options.
- **Approvals.** `Nd` means N business days (weekends skipped). Expired
  design-approval or escalation waits abort the run with
  `approval_timeout`/`escalation_timeout`; the resolved values are stored on
  the run, so `cf approve` sees the same deadline. `approvals.waiver` is
  resolved and stored the same way for the waiver-request deadline.
- **Waivers.** Only a human can waive a gate rule, with
  `cf approve --waiver <rule> --scope <file:line> --justification <text>
  [--approved-by <person>] [--milestone <id>]`. The record names the approver
  and reason and is listed in `report.json`/`report.md` and the PR body. The
  next gate run waives a failed gate when every finding is covered (a `*`
  scope covers the whole rule); skipped checks are never waived, and commit
  trailers list the waivers. With `--milestone` (or a matching failed gate)
  the command also returns an escalated milestone to `Gating` and the run to
  `Executing`, so `cf run --execute` re-runs the gates with the waiver.
  Agents can call `request_waiver`; a pending request pauses the run before
  the next agent turn, is listed in `status`/the report, carries the
  `approvals.waiver` deadline and is expired by the next run or
  approval/report command. An approved waiver or expiry lets execution
  continue.
- **Budget.** The plan's block wins over the file. The wall-clock limit counts
  only active states; waiting for people is bounded by `approvals` instead.
- **Waiting-state reminders.** `cf remind` checks the run's confirmed PR and
  posts the §12.1 reminders that are due (design approval after 2 and 4
  business days, escalation after 1 and 2), mentioning CODEOWNERS users. A
  hidden marker on each comment makes the job idempotent. `cf remind --all`
  sweeps every run in the workspace.
- **Branch retention.** `cf cleanup` checks the run's confirmed PR; once it
  is merged or closed for `github.branch_retention_days` (default 30) it
  deletes only the run's `cf/<run>/*` branches through the GitHub API. The
  command is idempotent (a missing branch is already clean) and keeps the
  commit SHAs in the run store. `cf cleanup --all` sweeps every run in the
  workspace, so one scheduled invocation covers all runs.
- **GitHub push scheduling.** `push_on` selects the events that push:
  `milestone_done` (milestone and integration branches, PR open/update),
  `run_done` (final integration and marking the PR ready) and `escalation`
  (escalation branch and comment). It defaults to all three; an empty list is
  rejected. With `run_done` disabled the PR stays a draft even at `Done`.
- **Commit signing.** When `github.signing_key_ref` (or
  `--github-signing-key-file`) names an SSH private key, git signs every CF
  commit with it (design §10.4.2), including squashed integration commits; the
  pre-push check then rejects any unsigned commit since the base branch.
  `secretmanager://` references are rejected in this build; use a file path.
- **GitHub rulesets.** `cf rulesets` writes the main/release and `cf/**`
  payloads (review, CODEOWNERS, signatures, linear history, no force
  push/deletion; signed commits on `cf/**`). Apply them with an admin token;
  the CF bot has no administration permission by design.
- **GitHub pre-push.** The checks run on the exact commit before every flow
  push; a rejection records no intent and escalates the run. If the base
  moved, the orchestrator rebases the integration branch, re-runs the gate
  plan on the new head and records those results before pushing; a conflict
  or failing re-gate escalates. Configured `protected_paths` are additive:
  the standard set cannot be removed.
- **GitHub PR policy.** `draft_until_done: false` opens the PR ready (and
  requests reviewers at creation when enabled); labels are applied
  idempotently; the PR body is built from the run summary (gates,
  traceability, defects, waivers, cost) and links `runs/<run>/report.md`.
- **Reference docs.** `reference_docs` mirrors the approved manifest into
  `runs/<run>/reference-docs` once per run and pins the dependency versions
  found in the target checkout. Archive sets are unpacked and filtered by their
  `extract` patterns; Doxygen sets clone their `repo`/`ref` and generate HTML. A set whose mirrored version does not match the project
  pin is skipped (or fails the run with `on_missing_version: fail`);
  `lookup_reference` then reports it as not covered instead of returning
  another version.
- **RAG.** `top_k` is the default for `search_guidelines` and the refiner
  context size; `models.embeddings` selects the provider/model for hybrid
  retrieval (it must have a matching `providers` entry). `cf ingest` accepts
  PDF and DOCX files with a `<file>.meta.yaml` sidecar (poppler `pdftotext`,
  Pandoc). Hybrid weights scale the two reciprocal-rank-fusion lists;
  `precedence` decides conflict winners, with unlisted types appended in the
  built-in order.
- **Pattern conformance.** When the pattern catalog is loaded, refinement
  requires every ADR to carry a `pattern_id` that exists in the catalog;
  missing or unknown ids fail the design before the approval gate.
- **Design changes.** `cf approve --request-changes --reason <text>` moves
  `AwaitingDesignApproval` back to `Refining`, archives the rejected
  `design.{json,md}` files and records the feedback; the next refinement
  prompt includes it.
- **Pinned profile.** `gates.profile` selects a toolchain profile by id
  instead of resolving from the plan's `target`; the pinned profile must
  still declare the plan's exact language/build-system/test-framework
  combination (FR-18).
- **Review severities.** `review.blocking_severities` selects which finding
  severities from `submit_review` block the milestone; a verdict with no
  structured findings falls back to its explicit verdict.
- **Stage-3 gates.** A profile may declare trusted `steps` under
  `common.coverage` and `common.mutation`; they run only after every stage-1
  and stage-2 gate passed. `gates.coverage` and `gates.mutation` override the
  profile thresholds (`line`/`branch` are ratios), and
  `mutation.enabled: false` removes the mutation gate. The steps can use
  `{coverage_line}`, `{coverage_branch}` and `{mutation_min_score}`. The
  Python profile ships line-and-branch coverage steps; C/C++/Go coverage and
  mutation runners are profile-author supplied.
- **Gate resources.** `sandbox.cpu`, `memory_gb` and `job_timeout_min` apply
  to gate jobs (container/namespace limits and step timeout).

All documented `cf.yaml` sections are now accepted; unsupported values are
rejected (for example, `sandbox.network` only accepts `deny`). See
[§9](#9-known-gaps-and-roadmap).

---

## 6. MCP tool server

`cf-mcp` implements the design's tool surface (§9.1) over MCP stdio, filtered by the
agent role:

```bash
cf-mcp --role coder --run R12 --milestone M3 \
       --db cf-work/runs/R12/run.db \
       --docs cf-work/docs \
       [--codeintel-db cf-work/runs/R12/codeintel.db] \
       [--rules-dir /cf/rules] \
       [--top-k 8] [--rag-index hnswlib|faiss|faiss-ivfpq|brute-force] \
       [--rag-bm25-weight 0.4 --rag-vector-weight 0.6] \
       [--rag-precedence policy,requirements,architecture,design,guideline,pattern,external_reference]
```

The run process forwards the `rag` settings from `cf.yaml` to every role server;
`--top-k` also supplies the default when `search_guidelines` omits `top_k`.

| Tool | Roles | Notes |
|---|---|---|
| `search_guidelines`, `get_document`, `get_requirement`, `get_design`, `lookup_reference` | all | RAG-backed; hits carry citations, precedence conflicts and `unresolved_conflict` flags; `lookup_reference` defaults to the run's pinned version and returns `covered:false` for mirror gaps |
| `get_pattern` | coder, reviewer | Pattern catalog entries |
| `get_gate_results`, `list_defects` | all | Read from the run store |
| `file_defect`, `verify_defect` | qa | Fingerprint de-duplication; recurring defects are marked `REOPENED` |
| `submit_work` / `submit_review` / `submit_tests` | coder / reviewer / qa | Structured turn endings |
| `dispute_test` | coder | Records a `DISPUTED` defect against a protected test and ends the turn; QA decides immediately (fix or reject), a second dispute of the same test escalates (design §6.9, §12) |
| `find_symbol`, `get_callers` | all | Query the cf-codeintel symbol index when `--codeintel-db` is given; `TOOL_UNAVAILABLE` otherwise |
| `get_context` | coder, reviewer | Packed code context within a token budget plus relevant ADR/contract text; the task seed is a symbol name or an indexed file/component path |
| `request_waiver` | coder, qa | Records a pending human waiver request with the configured business-day deadline; never grants it |
| `run_checks` | coder, qa | Dry-runs declared profile gates on the agent worktree (build/format/guardrails/analysis/tests); no gate result is recorded. `TOOL_UNAVAILABLE` when the role server has no profile. `--rules-dir` supplies the trusted `swt.*` rules copied into the run |

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
setups). Generated Doxygen sets clone the approved `repo` at `ref`, run Doxygen
and index the cleaned HTML. Archive sources with `extract` patterns are
unpacked with the system `tar`/`unzip`; matching HTML/Markdown documents are
cleaned, injection-scanned and written as separate front-matter documents
(`<id>-<version>-N.md`) alongside the raw archive. The checked-in manifest carries placeholder
URLs/checksums until the real approved sets land.

With `reference_docs` configured, `cf run --execute` mirrors the manifest into
`runs/<run>/reference-docs` once, pins the versions named by the target
checkout's dependency files in `runs/<run>/reference-versions.json`, and indexes
the mirror in the refiner, question answering and every role server. A version
mismatch skips that set (or aborts the run with `on_missing_version: fail`), and
the skipped sets are recorded in the same JSON file.

---

## 8. Testing

```bash
ctest --test-dir build/debug --output-on-failure     # 784 unit tests
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
binary. Plans with TSan tests need ASLR entropy lowered when the QA agent runs
its own TSan build: run the whole bench under `setarch $(uname -m) -R` (or
apply the `vm.mmap_rnd_bits` sysctl from §4). Hidden TSan steps can instead
declare `disable_aslr: true`, which the evaluator applies to that step.
`cf-eval bench --agent-sandbox-namespaces --ns-agent <path> --opencode <real-ELF>`
runs the same plan with agent servers in the namespace sandbox instead of
Docker. Pass the real opencode ELF (not a wrapper that references hidden host
paths) and make the target toolchain visible under the sandbox's `/usr` or
mount it with `--ns-mount` and `--ns-path` (and `--ns-env` for settings).

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
the labeled set now has 205 queries, above the design's 200-per-collection minimum.

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
  server, with real git snapshots, gates and integration. The same path also
  runs the opencode servers in the daemon-free namespace sandbox when
  `--agent-sandbox-namespaces`/`agent_sandbox_namespaces` is set; the Phase 0b
  benchmark has already run end to end with real models.
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
  escape suite covers it; recovery also force-removes any container left
  behind by a killed orchestrator, using the `cf.run` label.
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
  rule), not because of the executor. Agent servers use the same executor with
  `cf-ns-agent`: the host API port is bridged into the namespace over a Unix
  socket, and model traffic goes through a loopback proxy to the
  allow-listing host proxy over a second Unix socket. The netns has only
  loopback, so direct egress remains kernel-blocked. The host side installs
  `cf-mcp` and the opencode binary read-only inside `/cf/bin` and gives the
  namespace TSan/seccomp the same policy as gate jobs. It uses the host's
  `/usr` toolchain: binaries outside `/usr` (for example a Maven or Go
  installation under `/home`) must be on a mounted path or installed in the
  host system, or mounted with `--ns-mount HOST:TARGET[:ro]` and put on the
  namespace `PATH` with `--ns-path`; `--ns-env KEY=VALUE` adds toolchain
  settings such as `MAVEN_OPTS` or `GOMODCACHE`. Pass the real opencode ELF,
  not a wrapper that references hidden host paths.
- **Company guardrail checks (`cf-swt-*`).** The first checks,
  `cf-swt-no-raw-new`, `cf-swt-no-raw-delete`, `cf-swt-no-raw-malloc` and
  `cf-swt-no-detached-thread` (CG-4.2), `cf-swt-no-hardcoded-secret` and
  `cf-swt-no-bool-param`, `cf-swt-no-c-style-cast`,
  `cf-swt-no-using-namespace-in-header`, `cf-swt-no-goto`,
  `cf-swt-no-reinterpret-cast`, `cf-swt-no-const-cast`,
  `cf-swt-no-raw-array`, `cf-swt-no-unsafe-c-function`,
  `cf-swt-no-multiple-inheritance`, `cf-swt-no-public-data-member`,
  `cf-swt-no-public-static-data`, `cf-swt-no-sleep-in-test` and
  `cf-swt-no-weak-hash` are implemented in `cf-tidy` with fixture tests, and
  the C/C++ profile runs them after the standard clang-tidy check set (18
  company checks total). The
  pinned LLVM package is static, so `cf-tidy` is a clang-tidy-compatible gate
  driver rather than a loadable `.so`; a plugin build can be added for
  shared-LLVM distributions. A starter `rules/guardrails.yaml` catalog records
  automated and reviewer-only rules (`cf-tidy --list-checks` lists the compiled
  checks); the full guideline mapping remains.
- **Coverage and mutation gates.** The stage-3 runner, threshold placeholders
  and built-in coverage parser (`coverage-json`, `go-cover`, `lcov`,
  `llvm-cov-json`, `jacoco-xml`) are implemented. The Python, Go, TypeScript,
  C/C++ (CMake/Meson/Make) and Java (Maven/Gradle) profiles enforce their
  configured thresholds. Python mutation runs through a mutmut wrapper and the
  gate enforces its `mutmut-json` score; Go and Java mutation run through
  gremlins and PIT, TypeScript/JavaScript through Stryker and C/C++ through
  Mull.
- **GitHub operations.** `cf-github` provides the REST client (pull requests,
  issues, comments, hidden `cf-op` markers), the full §10.4.6 pre-push check
  set (branch policy, protected paths, size, binaries, required trailers,
  secret scanning with findings that never carry the secret, and dependency
  licence checks against a local licence database and allow-list; the flow
  runs it on the exact commit before every push and escalates a rejection)
  and `cf-orch` provides the dispatcher handlers (`push` with compare-and-swap,
  never over a moved remote; `pr_create`; `pr_update` incl. closing;
  `issue_create`; `comment`), each idempotent and its own reconciler, plus
  `GithubFlow`, which the run process calls after every step: Done milestones
  push to `cf/<run>/<m>` and `cf/<run>/integration`, a PR opens on the first
  milestone (draft while in progress unless `pr.draft_until_done` is false),
  its body is built from the run summary (gates, traceability, defects,
  cost) and later states update it (skipping no-op syncs), configured
  `pr.labels` are applied, `Done` marks it ready, escalation pushes
  `cf/<run>/escalation/<at>` and comments the bundle link, and abort closes
  the PR. The GitHub App identity signs an RS256
  JWT in memory and exchanges it for short-lived installation tokens, cached
  and refreshed before expiry; the CLI loads the key from a file
  (`--github-key-file`) and wires everything through. Defects are exported to
  GitHub Issues too (`DefectExporter`, §6.10): one issue per defect labelled
  `cf-defect`, `severity:*`, `milestone:*` and `requirement:*`, with the hidden
  fingerprint marker so a recurring defect reuses the existing issue across
  runs; status changes sync (close on VERIFIED, reopen on REOPENED), a human
  closing the issue is pulled back as `WONT_FIX`, bodies are secret-scanned
  and restricted runs post only IDs and the report link. Marking the PR ready
  requests the CODEOWNERS reviewers unless `pr.request_codeowners` is false
  (design §10.4.4): the handler reads
  `.github/CODEOWNERS`, `CODEOWNERS` or `docs/CODEOWNERS` from the repository
  and requests users and teams on the pull request, again on reconcile after a
  crash (requesting is idempotent); a missing file simply means no reviewers.
  When the base moves, the run process rebases the integration branch and
  re-runs the integration gates before pushing. A conflict starts a coder
  Fixing turn on the integration worktree; the driver stages the resolution
  only after the conflict markers are gone, continues the rebase and re-gates.
  A failed resolution or re-gate escalates. Remaining for full §10.4 fidelity:
  applying the generated rulesets from an admin context.
- **Waiver flow (§10.2, §12.1).** Human waivers are recorded by
  `cf approve --waiver`, persisted in the run store and listed, with the
  approver and justification, in the report and the PR body; agents can
  request one but have no way to grant it. The gate loop consumes them, an
  escalated milestone goes
  back to `Gating`, and commits carry `CF-Waivers`. The resolved
  `approvals.waiver` deadline is stored on the run. A pending agent request
  pauses the run, is listed in `status`/the report, receives a reminder after
  one business day, and can be approved (`--waiver`) or refused
  (`--refuse-waiver`) by a human, or expires at its deadline.
- **Observability (§6.11, §14).** `cf-telemetry` installs a JSON formatter
  and a thread-local context, so every log line carries `run_id`,
  `milestone_id`, `agent` and `trace_id`; the run process and driver scope
  each agent turn and gate run. `report.json` carries the design's metric
  names and labels (gate pass ratio, iterations, open defects, LLM
  tokens/USD, escalations, RAG latency average/p95) and exports it as
  OTLP/HTTP JSON when `OTEL_EXPORTER_OTLP_ENDPOINT` is set. Direct `cf-llm`
  calls (planner and embeddings) are archived under `runs/<run>/llm`, and
  opencode HTTP calls and SSE events are archived under
  `runs/<run>/opencode`; both use gzip plus AES-256-GCM with
  `CF_ARCHIVE_KEY` (64 hex characters). Direct-call refs go into the cost
  ledger, and `cf replay` re-sends one to the same or another model.
- **Model benchmark (Phase 0b).** `cf-eval bench` implements the benchmark: the
  candidates in `eval/benchmarks/fireworks.yaml` run through the real pipeline on
  the evaluation plans, with hidden acceptance tests and scored reports under
  `bench-out/`; it needs `FIREWORKS_API_KEY` and the pinned opencode. The full
  benchmark (3 candidates × 7 plans, C++ T1/T2/T3, security, Python, Go and
  crash recovery) completed: the pinned candidate finished 6 of 7 runs Done
  with every hidden test green (79/79) at $0.16 per milestone, the best
  completion of the three; the pins in `third_party/VERSIONS` carry the
  provenance note. Aborting an escalated run (`cf abort`) withdraws the
  designs it published, so later runs cannot build on an abandoned design.
  The T3 plan escalated for every candidate in that run; the
  fixes it exposed (a separate revalidation build directory, no-op
  submissions for already-integrated milestones, QA repair of protected tests
  invalidated by the interface change, and broader build-artifact detection)
  now let T3 complete end to end with 12/12 hidden tests. The expanded
  19-plan runs completed for all three candidates; the pinned candidate is
  best at 18/19 runs Done, 0.9474 milestone completion and 0.8816 hidden-test
  pass ($0.1490/milestone). Plan/seed fixes for the remaining T3, TypeScript
  and Python cases were rerun individually green. Reliability scenarios
  inject a crash: a hidden spec's `crash_after_milestone` (or
  `crash_after_seconds`, optionally `crash_count` for repeated kills) makes
  the bench SIGKILL the run process mid-run, run `cf resume` (lease takeover,
  stale locks, in-flight operations, worktree reset) and continue;
  `rel-orchestrator-crash` kills at a milestone boundary and
  `rel-crash-mid-turn` kills twice mid-turn (exercising the checkpoint reset
  of a dirty worktree). Both verify the run still reaches Done with no
  duplicated commits or operations.
  TSan-involving plans need `setarch $(uname -m) -R` on hosts with high ASLR
  entropy (same note as §4); hidden TSan steps can declare `disable_aslr` to
  have the evaluator apply it. The expanded 19-plan host run used `setarch`
  and Maven on `PATH`; namespace-sandboxed runs now
  complete for Go (`t1-go-lru`, 14/14 at $0.2357), Java (`t1-java-stack`,
  8/8 at $0.2236 with the GLM coder; the default coder remains flaky on
  submission) and TypeScript (`t1-typescript-slug`, 4/4 at $0.2641, with an
  offline npm cache and a small seed that pins the build/test tooling).
  The next benchmark pass should rerun the full three-candidate matrix on the
  fixed plans.
- **Reliability fault injection.** The driver records a checkpoint before
  every agent turn (design section 6.12.3), and the bench kills the run
  process at a milestone boundary or mid-turn (repeatedly, with
  `crash_count`), resumes it (lease takeover, stale locks, in-flight
  operations, worktree reset to the checkpoint) and continues;
  `rel-orchestrator-crash` and `rel-crash-mid-turn` verify the run still
  reaches Done with no duplicated commits or operations. Startup recovery
  (design section 6.12.4) also runs automatically at the beginning of
  `cf run --execute`, with the GitHub handlers wired in as reconcilers, so
  operations left `STARTED` or `NEEDS_RECONCILE` by a killed orchestrator
  are reconciled before any new effect; recovery waits out the in-flight
  grace period (renewing the lease every 10 s while it waits), so an old
  request can no longer land after the world was read, and it removes
  orphaned run containers by the `cf.run` label (gate jobs and agent
  servers). `cf resume` releases the lease it took so execution can follow
  immediately. The §6.12.7 fault-injection suite covers every GitHub
  operation kind (push, pr_create, pr_update, issue_create, issue_update,
  comment) for the crash after the intent (recovery lands the effect exactly
  once) and the crash between the effect and the confirm (reconciliation
  never duplicates), the stale compare-and-swap push (a late
  force-with-lease from a stalled orchestrator fails and leaves the branch),
  and the SIGSTOP takeover with a comment request in flight (the old
  epoch's confirm is fenced with STORE_STALE_EPOCH; a reconcile that finds
  the same comment twice deletes the later duplicate, §6.12.6 rule 4). For
  the local operations, a crash between the integration merge and its
  state record is recognized at the next Integrate by the CF-Milestone
  trailer on the integration branch (§6.12.5 merge row): confirmed as
  already integrated, never re-merged or spuriously escalated; agent turns
  are covered by the pre-turn checkpoint reset (§6.12.3) and the
  refs/cf/recovered parking, exercised end to end by the crash-injection
  reliability plans. Snapshot commits for tests, Code and Fix carry
  `CF-Phase`, so a restart after the commit but before the state record accepts
  the no-change retry; sandbox timeout/kill cleanup is covered by the escape
  suites. A broader kill matrix remains.
- **RAG production index.** hnswlib is the default vector index (brute force
  stays behind the same `VectorIndex` interface and is selectable with
  `rag.index: brute-force`); optional FAISS HNSW and IVF-PQ indexes are enabled
  with `FAISS_ROOT` and `rag.index: faiss`/`faiss-ivfpq`. IVF-PQ now uses a
  bounded, configurable training sample (65,536 by default), adaptive or
  explicit nlist, nprobe and nbits; persistence and tuning for collections
  over 5M chunks remain pending. PDF/DOCX ingestion uses the external converters
  (`pdftotext`/`pandoc`) and a metadata sidecar. The labeled retrieval set has
  205 queries (above the 200-per-collection target) and the adversarial
  conflict set is in place.
- **System-level validation loop.** `WorkspaceDriver::validate_system` drives a
  real QA system-validation session on the integration worktree and routes
  defects back to Executing. The golden suite includes cross-component C++,
  Go, Java, Python and TypeScript plans; expanding the scenario matrix remains.

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
tests/unit/                 # GoogleTest unit tests (784)
tests/harness_compat/       # Phase 0a compatibility spike (H1-H15)
eval/                       # golden suite manifest/plans, hidden acceptance tests,
                            # benchmark configs and RAG evaluation sets (cf-eval)
reference-docs/manifest.yaml
catalog/patterns/           # curated pattern catalog (YAML, design §6.2)
prompts/                    # versioned role prompt templates
profiles/                   # toolchain profiles (YAML)
rules/semgrep/              # company swt.* rules for non-C/C++ targets
rules/guardrails.yaml       # automated/reviewer-only rule coverage
rules/scripts/              # trusted gate helpers (all-language coverage and mutation)
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
  (`FIREWORKS_API_KEY`, `VLLM_API_KEY`); the LLM/opencode archive key comes
  from `CF_ARCHIVE_KEY` (64 hex characters), and
  `OTEL_EXPORTER_OTLP_ENDPOINT` enables the metrics export.

---

## 12. License

MIT — see [`LICENSE`](LICENSE).
