# AGENTS.md

## Process safety (hard rule)

The opencode session you are running in **is itself a process named
`opencode`**, and every bash command runs as a descendant of it. Name-based
kills therefore kill the session:

- NEVER run `pkill`, `killall`, `pkill -f opencode`, `kill $(pgrep …)`, or
  kill any pid produced by `pgrep -x opencode` / `ps -C opencode` — those also
  match the session process (and `opencode serve` children share the same
  name).
- Kill only explicit numeric pids you started yourself, and never a pid that
  is an ancestor of your shell.
- To clean up opencode servers leaked by an interrupted `cf run --execute` or
  `cf-eval bench`, use `scripts/kill-cf-servers.sh`. It matches only
  framework-spawned `opencode serve` processes (`CF_ROLE` in their
  environment), never the session. Prefer wrapping long runs in `timeout` and
  letting the run process stop its own servers (it does on normal exit; the
  kernel takes them down if the run process dies).
- `.opencode/opencode.json` denies variable-based and name-based kills as a
  second line of defence; do not work around a denial.

## Project conventions

- Strict C++17, `-Werror`, no exceptions across module boundaries: fallible
  functions return `cf::Result<T>` / `cf::Status`. `README.md` and
  `Coder_Framework_Design_Document.md` are the source of truth; code comments
  cite the design section.
- Build and test:
  `cmake --build build/debug -j"$(nproc)"` and
  `ctest --test-dir build/debug --output-on-failure`.
- Long real-model runs (`cf-eval bench`, `cf run --execute`) need
  `FIREWORKS_API_KEY` and the pinned opencode binary; keep them bounded with
  `timeout` and write logs under `bench-out/` or `/tmp/opencode/`.
- Never commit secrets.
