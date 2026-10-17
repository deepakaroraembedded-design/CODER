#!/usr/bin/env bash
# Stop opencode servers spawned by the Coder Framework (after an interrupted
# `cf run --execute` / `cf-eval bench`).
#
# The user's opencode session is itself a process named `opencode`, so
# name-based kills (`pkill opencode`, `kill $(pgrep -x opencode)`) kill the
# session. This script never matches by name alone: it only stops processes
# that carry the framework server fingerprint - `opencode` AND `serve` on the
# command line AND `CF_ROLE` in the environment (set by
# cf::orch::OpencodeRoleFactory) - and it refuses to touch the current process
# tree. Servers whose owning cf/cf-eval process is still running are left
# alone unless --force is given.
#
# Usage: scripts/kill-cf-servers.sh [--dry-run] [--force]
set -euo pipefail

dry_run=0
force=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) dry_run=1 ;;
    --force) force=1 ;;
    *)
      echo "usage: $0 [--dry-run] [--force]" >&2
      exit 2
      ;;
  esac
done

# The ancestors of this shell (the opencode session tree): never touch them.
ancestors=" "
pid=$$
while [[ -n "$pid" && "$pid" != "0" && "$pid" != "1" ]]; do
  ancestors+="$pid "
  pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ' || true)
done

# True when a cf or cf-eval ancestor of the given pid is still alive.
owner_alive() {
  local pid=$1 argv0
  while [[ -n "$pid" && "$pid" != "0" && "$pid" != "1" ]]; do
    argv0=$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | head -n 1 || true)
    case "${argv0##*/}" in
      cf | cf-eval) return 0 ;;
    esac
    pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ' || true)
  done
  return 1
}

pids=()
for proc in /proc/[0-9]*; do
  pid=${proc#/proc/}
  [[ -r "$proc/cmdline" && -r "$proc/environ" ]] || continue
  cmd=$(tr '\0' ' ' < "$proc/cmdline" 2>/dev/null || true)
  case "$cmd" in
    *opencode*serve*) ;;
    *) continue ;;
  esac
  tr '\0' '\n' < "$proc/environ" 2>/dev/null | grep -q '^CF_ROLE=' || continue
  case "$ancestors" in *" $pid "*) continue ;; esac
  if (( ! force )) && owner_alive "$pid"; then
    echo "skip $pid (its cf/cf-eval owner is still running): $cmd"
    continue
  fi
  if (( dry_run )); then
    echo "would stop $pid: $cmd"
  else
    echo "stopping $pid: $cmd"
  fi
  pids+=("$pid")
done

if (( ${#pids[@]} == 0 )); then
  echo "no framework opencode servers found"
  exit 0
fi
if (( dry_run )); then
  exit 0
fi

sleep 1
for pid in "${pids[@]}"; do
  if kill -0 "$pid" 2>/dev/null; then
    echo "forcing $pid"
    kill -KILL "$pid" 2>/dev/null || true
  fi
done
