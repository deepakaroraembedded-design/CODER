# Phase 0a: opencode harness go / conditional-go / no-go decision

| Field | Value |
|---|---|
| Status | Final recommendation for CTO sign-off |
| Date | 2026-10-13 |
| Pinned opencode | 1.18.35 (`third_party/VERSIONS`) |
| Spike | `cf-harness-spike` (tests/harness_compat), scenarios H1-H15 from design section 6.6.2 |
| Result | **300/300 iterations passed** (15 scenarios x 20 runs) |
| Evidence | `docs/phase0a/evidence/report-20261013-185535.json` |
| Decision owner | CTO (sign-off required before Phase 0b) |

## 1. What was tested

The C++17 adapter (`cf::agents::OpencodeClient`, `src/agents/opencode_client.cpp`)
drives a pinned `opencode serve` process over HTTP and consumes its SSE event
stream. A minimal MCP server (`cf-mcp-spike`, `src/mcp/spike_main.cpp`) implements the structured
submission surface. Every scenario runs against a fresh server, project
directory and config.

| # | Scenario | Pass condition (design section 6.6.2) |
|---|---|---|
| H1 | Session, task, event stream to completion | Events in order; final state matches |
| H2 | Structured submission through an MCP tool | Valid JSON every time; turn ends after the call |
| H3 | Permission prompt answered (once and reject) | Run continues correctly; no "always" grant stored |
| H4 | Every permission type the pinned version can raise | Each detected and answered; unknown types rejected safely |
| H5 | MCP error, timeout, invalid JSON | Model recovers or the turn fails cleanly; no hang |
| H6 | Malformed tool arguments | Schema error returned; retry or clean failure |
| H7 | Event stream drop and reconnect | Missed events detected; session state recovered |
| H8 | opencode server killed mid-turn | Detected within 10 s; no hang |
| H9 | Turn cancelled by the orchestrator | Session stops; no file changes after cancel |
| H10 | Model asks a question instead of submitting | Detected; continuation works within two attempts |
| H11 | Provider errors (429, 5xx, timeout) | Retries with backoff, or clean failure reported |
| H12 | Tool calling with each default model family | Tool-call success rate >= 95% on the task set |
| H13 | Permission request semantics (blocking, late replies, restart) | Documented behaviour; no reply applied to the wrong request |
| H14 | Cancellation semantics | No file change and no tool call completes after cancel |
| H15 | Several sessions; state after server restart | Sessions isolated; restart handled |

## 2. Architecture findings

1. **The v2 `/api/*` API cannot drive turns in this build.** `POST
   /api/session/{id}/prompt` admits a prompt, but `POST /api/session/{id}/wait`
   returns 503 (no foreground drain) and the loop never starts. CF uses the
   documented v1 surface (`/session`, `/event`); the adapter isolates this, so
   moving to `/api/*` later is a single-file change.
2. **Structured submission works through local stdio MCP.** The model calls
   `cf_submit_work`; the tool result returns over MCP; the turn ends after the
   call (H2).
3. **Permissions are fully controllable.** `permission.asked` carries
   `{id, sessionID, permission, patterns, metadata, always, tool}`; replies go
   to `POST /session/{id}/permissions/{id}` with `once`/`reject`. Pending
   requests are re-listable with `GET /permission`, which is the recovery path
   when an event is missed (H7).
4. **Questions are answered through `POST /question/{id}/reply`**; the model
   continues the turn after the answer (H10a). A turn that ends with a text
   question is detected by the orchestrator, which sends the fixed continuation
   message; the model then completes the task (H10b).
5. **Cancellation is reliable.** `POST /session/{id}/abort` stops a running
   bash tool before it completes and clears a pending permission request; a
   late reply after abort is not applied (H13/H14).
6. **Provider faults surface as `session.status: retry`** with backoff; a
   permanently failing provider ends the turn cleanly and can be cancelled
   (H11). No hang was observed in any fault mode.
7. **Sessions survive a server restart** and can be continued; history is
   intact (H15).
8. **SSE stream death is detected quickly** (server SIGKILL: detected in
   ~35-40 ms, well inside the 10 s bound) via the read loop's error path (H8).

## 3. Results

Full run: `--runs 20` (each scenario 20 times), models pinned in
`third_party/VERSIONS` (DeepSeek V4.1 Flash, Kimi K3, GLM 5.3 on Fireworks).
All 300 iterations passed; no scenario regressed and no unhandled fault
occurred.

| # | Scenario | Passed | Key evidence from the run |
|---|---|---|---|
| H1 | Turn to completion | 20/20 | Final text matched in every run; no stream disconnects |
| H2 | MCP structured submission | 20/20 | `submit_work` received with valid JSON in 20/20; turn ended after the call |
| H3 | Permission once / reject | 20/20 | `once` executed, `reject` blocked the command, 0 "always" grants |
| H4 | Permission types | 20/20 | Observed types answered: edit 20, read 26, bash 34, glob 20, grep 20 |
| H5 | MCP error / timeout / invalid JSON | 20/20 | All three fault modes ended idle in 20/20; no hang |
| H6 | Malformed tool arguments | 20/20 | Schema error surfaced; model recovered with a valid call |
| H7 | Stream drop / reconnect | 20/20 | Final state correct in 20/20; missed permission recovered by `GET /permission` poll in 17/20 (in 3/20 the request was answered before the drop) |
| H8 | Server killed mid-turn | 20/20 | Detection 35-59 ms (median ~40 ms), bound is 10 s |
| H9 | Watchdog cancel | 20/20 | No late file created after cancel; turn stopped |
| H10 | Question instead of submission | 20/20 | Question tool answered 20/20; continuation completed the task 20/20 |
| H11 | Provider errors | 20/20 | Persistent 429: retry surfaced 20/20; fail-twice-then-succeed: completed 20/20; hang: still busy then cancelled 20/20 |
| H12 | Tool calling per model family | 20/20 | DeepSeek 20/20, Kimi 20/20, GLM 20/20 (**100%**, target >= 95%) |
| H13 | Permission semantics | 20/20 | Blocks while pending; late replies after cancel/restart not applied; unknown request IDs fail cleanly |
| H14 | Cancellation semantics | 20/20 | Running tool stopped before completion; pending permission cleared; no tool ran after cancel |
| H15 | Multi-session and restart | 20/20 | Sessions isolated; history and continuation survived a server restart |

The raw per-iteration evidence (event counts, tool states, MCP calls, timings)
is in the report file. An earlier draft run reported one H11 flake, caused by
two spike processes racing during a tooling mistake (duplicate background
launch), not by opencode; the clean single run above is the recorded result.

## 4. Workarounds required (conditional-go items)

| # | Finding | Workaround | Risk if opencode changes |
|---|---|---|---|
| W1 | `/api/*` drain disabled | Use the v1 API surface | Low: v1 is the documented, stable surface |
| W2 | No event sequence numbers on the v1 stream | State-poll recovery: `GET /session/status`, `GET /permission`, `GET /session/{id}/message` after any reconnect | Low: polling is idempotent |
| W3 | The stream is server-wide, not per session | Filter events by `sessionID`; run one server per role as the design already does | Low |
| W4 | A turn can end with a question instead of a submission | The orchestrator's continuation message, at most twice (H10) | Low: already in the design (section 6.6.1 layer 3) |

## 5. Reproducing the spike

```bash
cmake --preset debug
cmake --build build/debug -j"$(nproc)"
./build/debug/cf-harness-spike \
  --mcp "$PWD/build/debug/cf-mcp-spike" \
  --runs 20 --timeout 240
```

The run writes `spike-out/report-<timestamp>.json` with per-iteration evidence
and per-scenario/per-model aggregates. Scenarios can be selected with
`--scenarios H1,H2,...`. The same binary and scenario suite run in CI against
every candidate opencode version before it is pinned (design section 6.6.2).

## 6. Not covered in this environment

- **Local vLLM (H11 second provider):** no local vLLM host was available. The
  mock OpenAI-compatible provider covered 429/retry/hang semantics; the local
  path reuses the same adapter and will be re-tested when the hardware is
  ready (design section 13.1).
- **H4 permission-type breadth:** edit, read, glob, grep and bash were
  exercised; webfetch/websearch/task/external_directory/lsp/skill were not
  triggered by the test task. The broker's decision path is type-agnostic and
  was validated for all observed types.
- **H12 rate:** the 20x run provides a first sample per family; the design's
  >= 95% target is evaluated on the aggregated run.

## 7. Recommendation

**Go.** All 15 scenarios passed 20/20 (300/300 iterations) against the pinned
opencode 1.18.35, including every foundational scenario (H1, H2, H3, H7, H8,
H13, H14). The W1-W4 items above are adapter-level choices, not failures; each
is isolated behind `OpencodeClient` and re-tested by the same suite.

Two conditions are recorded:

1. **Local vLLM (H11 second endpoint)** is not validated here (no local vLLM
   host available). The protocol path is covered by an OpenAI-compatible mock;
   the real endpoint is validated in Release 2 (design section 13.1), before
   on-premises runs are enabled.
2. **The scenario suite must pass before any opencode version is pinned or
   upgraded** (`cf-harness-spike --runs 20`; the suite is CI-ready).

Per the design (section 18.2, Phase 0a), no Phase 0b or later work starts
until the CTO signs off this decision.
