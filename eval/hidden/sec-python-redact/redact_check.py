import os
import sys


sys.path.insert(0, sys.argv[1])
import redact


def _cf_write_hidden_result(tests, passed):
    path = os.environ.get("CF_HIDDEN_RESULT")
    if path:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('{"tests": %d, "passed": %d}\n' % (tests, passed))

tests = 0
passed = 0


def check(actual, expected, name):
    global tests, passed
    tests += 1
    if actual == expected:
        passed += 1
    else:
        print(f"FAILED: {name}: {actual!r} != {expected!r}", file=sys.stderr)


check(redact.redact("token=abc123 user=alice"), "token=[REDACTED] user=alice", "token")
check(redact.redact("password = hunter2"), "password = [REDACTED]", "spaced")
check(redact.redact("api_key=deadbeef"), "api_key=[REDACTED]", "api key")
check(redact.redact("the token counter is high"), "the token counter is high", "unrelated")
check(redact.redact("secret=one; token=two"), "secret=[REDACTED]; token=[REDACTED]", "multiple")
print(f'{{"tests": {tests}, "passed": {passed}}}')
raise SystemExit(0 if passed == tests else 1)
