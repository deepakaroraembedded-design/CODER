# Phase 0a: opencode 1.18.35 API recon notes

Source of truth: the OpenAPI 3.1 spec served by the pinned binary at
`GET /doc` (saved by the spike to `spike-out/openapi-1.18.35.json`).
Verified against a live server: `opencode serve --port <p> --hostname 127.0.0.1`.

## APIs present

The pinned version exposes two API families on the same server:

| Family | Prefix | Used by CF? | Notes |
|---|---|---|---|
| v1 | `/session`, `/event`, `/session/:id/message`, `/session/:id/permissions/:id` | **yes** | documented on opencode.ai; exercised end to end by scenarios H1-H15 |
| v2 | `/api/*` | no | explicit permission/question endpoints and a durable per-session event stream, but the session drain is not enabled in this build |

**Phase 0a finding:** the v2 family is present but not usable for driving turns in
1.18.35. `POST /api/session/{id}/prompt` admits the prompt, but
`POST /api/session/{id}/wait` returns 503 ("no foreground drain") and the agent
loop never starts. CF therefore uses the v1 surface, which the harness
compatibility suite exercises end to end (H1-H15). The adapter keeps the
protocol in one place, so switching to the v2 family later is an internal change.

The v2 endpoints are still useful for state polling: `GET /api/session/{id}/history`
and the `/api/session/{id}/event` stream are documented here for the migration.

## Endpoints used by the adapter (v1)

| Method | Path | Purpose |
|---|---|---|
| GET | `/global/health` | health + version (`{"healthy":true,"version":"1.18.35"}`) |
| GET | `/config/providers` | provider/model inventory |
| POST | `/session` | create session (`{}`; the model is passed per prompt) |
| GET | `/session/{sessionID}` | session info (cost, tokens, model) |
| GET | `/session/{sessionID}/message` | projected messages: `[{info, parts}]` |
| GET | `/session/status` | status map `{sessionID: {type: idle|busy|retry, ...}}` |
| POST | `/session/{sessionID}/prompt_async` | admit a prompt; body `{parts:[{type:"text",text}], model:{providerID,modelID}, agent?}`; 204 |
| POST | `/session/{sessionID}/abort` | cancel the running turn |
| POST | `/session/{sessionID}/permissions/{permissionID}` | body `{response:"once"|"always"|"reject"}` |
| GET | `/permission` | list pending permission requests (state-poll recovery) |
| POST | `/question/{requestID}/reply` | body `{answers:[[label,...],...]}` |
| POST | `/question/{requestID}/reject` | reject a question |
| GET | `/event` | server-wide SSE stream |
| GET | `/mcp` | MCP server status map |

Session IDs are `ses...`, messages `msg...`, permission requests `per...`,
questions `que...`, events `evt...`, parts `prt...`.

## Event envelope

SSE `data:` payloads carry:

```json
{ "id": "evt_...", "type": "<event.type>", "properties": { ... } }
```

`GlobalEvent` payloads wrap the same object under `payload` with
`directory`/`project`/`workspace` alongside it; the adapter unwraps `payload`.

### Event types relevant to the framework

Events observed on the v1 `/event` stream while driving turns (H1-H15):

| Event | Meaning |
|---|---|
| `server.connected` | stream established |
| `session.created` / `session.updated` / `session.diff` | session lifecycle |
| `session.status` | `idle` / `busy` / `retry` (with attempt, next, action) |
| `session.idle` | agent loop became idle (turn finished) |
| `session.error` | provider/auth/overflow/abort errors |
| `message.updated` | a message was created or updated (carries `info` with `role`) |
| `message.part.updated` | a part was created or updated (text, tool, reasoning, step-*) |
| `message.part.delta` | streamed text/reasoning delta |
| `permission.asked` / `permission.replied` | permission prompts |
| `question.asked` / `question.replied` / `question.rejected` | model questions to the user |
| `file.edited` | a file was edited |
| `mcp.tools.changed` | MCP server tools changed |
| `server.heartbeat` | SSE keep-alive comment/event |

The event union also contains the newer `session.next.*`, `permission.v2.*`
and `question.v2.*` variants; CF ignores them while on the v1 surface.

## Permission requests (v1, the flow CF uses)

```json
{
  "id": "per_...",
  "sessionID": "ses_...",
  "permission": "<permission type>",
  "patterns": ["echo hello"],
  "metadata": { "command": "echo hello" },
  "always": ["echo *"],
  "tool": { "messageID": "msg_...", "callID": "..." }
}
```

Replies: `once`, `always`, `reject`. The design forbids `always`
(section 6.6.1 layer 2, rule 5); the broker only ever sends `once` or
`reject`. Pending requests can be re-listed with `GET /permission`
(state-poll recovery after a missed event, scenario H7).

Configured permission types (from `PermissionConfig` in the spec): `read`,
`edit`, `glob`, `grep`, `list`, `bash`, `task`, `external_directory`,
`todowrite`, `question`, `webfetch`, `websearch`, `lsp`, `doom_loop`,
`skill` — each `ask` | `allow` | `deny`, with per-pattern rules for
`bash` and path rules for `edit`/`read`. Replies: `once`, `always`,
`reject`. The design forbids `always` (section 6.6.1 layer 2, rule 5);
the broker only ever sends `once` or `reject`.

## Questions

```json
{
  "id": "que_...",
  "sessionID": "ses_...",
  "questions": [ { "question": "...", "header": "...", "options": [ {"label": "...", "description": "..."} ], "multiple": false, "custom": true } ],
  "tool": { "messageID": "msg_...", "callID": "..." }
}
```

Reply: `{ "answers": [ ["label", ...], ... ] }` in question order.

## MCP configuration (local, stdio)

```json
{
  "mcp": {
    "cf": {
      "type": "local",
      "command": ["/abs/path/cf-mcp"],
      "enabled": true,
      "environment": { "CF_MCP_CALLS_FILE": "/abs/path/calls.jsonl" },
      "timeout": 5000
    }
  }
}
```

MCP tools are registered with the server name as prefix, e.g. the
`submit_work` tool on server `cf` appears as `cf_submit_work` and can be
restricted per agent with glob patterns (`"cf_*": true`).

## Model references

Model string form: `<providerID>/<modelID>`, e.g.
`fireworks-ai/accounts/fireworks/models/deepseek-v4p1-flash`.
The pinned version's `fireworks-ai` provider is authenticated through
`~/.local/share/opencode/auth.json`. Custom OpenAI-compatible providers
(the design's `fireworks` and `local-vllm` entries) are declared in config
with `npm: "@ai-sdk/openai-compatible"` and `options.baseURL`.

## Server process

- `opencode serve --port <p> --hostname 127.0.0.1` runs headless.
- `OPENCODE_SERVER_PASSWORD` enables HTTP basic auth (not used locally).
- `--pure` runs without external plugins.
- Version is reported by `/global/health`; the spike asserts 1.18.35.

## Items to confirm during the spike (H1-H15)

1. Whether `/api/event` emits the same envelope as `/api/session/{id}/event`
   (unwrapping `payload` is handled either way).
2. Exact SSE framing (multi-line `data:` joins) — parser handles both.
3. Permission request blocking semantics and late replies (H13).
4. Cancellation effects on running tools and pending permissions (H14).
5. Provider error surfacing (retry status vs `session.error`) (H11).
6. MCP tool-call failure/timeout/invalid-JSON behaviour (H5, H6).
