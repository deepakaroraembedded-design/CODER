#!/usr/bin/env python3
"""Hidden recovery checks for the crash scenario (design section 6.12.5).

After the injected SIGKILL and the resume, the run must be Done, every
milestone Done, no operation intent executed twice, nothing left mid-flight,
and the integration branch must carry exactly one merge commit per milestone
(a duplicated push/merge would show as a second commit).
"""
import argparse
import os
import sqlite3
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--worktree", required=True)
    args = parser.parse_args()

    tests = 0
    passed = 0

    def check(condition):
        nonlocal tests, passed
        tests += 1
        if condition:
            passed += 1

    database = os.path.join(args.run_dir, "run.db")
    connection = sqlite3.connect(database)

    # The run and every milestone reached Done.
    run = connection.execute("SELECT state FROM runs LIMIT 1").fetchone()
    check(run is not None and run[0] == "Done")
    states = [row[0] for row in connection.execute("SELECT state FROM milestones")]
    check(bool(states) and all(state == "Done" for state in states))

    # No operation intent appears twice (idempotency keys are unique) and
    # nothing is left mid-flight after recovery.
    duplicates = connection.execute(
        "SELECT COUNT(*) FROM (SELECT 1 FROM operations "
        "GROUP BY idempotency_key HAVING COUNT(*) > 1)").fetchone()[0]
    check(duplicates == 0)
    in_flight = connection.execute(
        "SELECT COUNT(*) FROM operations "
        "WHERE state NOT IN ('CONFIRMED', 'FAILED')").fetchone()[0]
    check(in_flight == 0)

    # The integration carries exactly one commit per milestone: a second
    # merge after the crash would duplicate the milestone's commit.
    log = subprocess.run(
        ["git", "-C", args.worktree, "log", "--format=%s"],
        capture_output=True, text=True, check=False)
    check(log.returncode == 0)
    subjects = log.stdout.splitlines()
    milestone_commits = [s for s in subjects if s.startswith(("feat(M", "fix(M"))]
    check(len(milestone_commits) == len(set(milestone_commits)))

    print('{"tests": %d, "passed": %d}' % (tests, passed))
    return 0 if passed == tests else 1


if __name__ == "__main__":
    raise SystemExit(main())
