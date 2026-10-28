import sys

sys.path.insert(0, sys.argv[1])
import stats

tests = 0
passed = 0


def check(condition, name):
    global tests, passed
    tests += 1
    if condition:
        passed += 1
    else:
        print(f"FAILED: {name}", file=sys.stderr)


summary = stats.summarize([1, 2, 3, 4])
check(summary["count"] == 4, "count")
check(summary["mean"] == 2.5, "mean")
check(summary["min"] == 1 and summary["max"] == 4, "min/max")
empty = stats.summarize([])
check(empty["count"] == 0, "empty count")
check("mean" not in empty and "min" not in empty and "max" not in empty, "empty has no stats")
try:
    stats.summarize([1, "two"])
    check(False, "rejects non-numeric")
except TypeError:
    check(True, "rejects non-numeric")

print(f'{{"tests": {tests}, "passed": {passed}}}')
raise SystemExit(0 if passed == tests else 1)
