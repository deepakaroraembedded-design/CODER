import sys

sys.path.insert(0, sys.argv[1])
import aggregate
import records

tests = 0
passed = 0


def check(condition, name):
    global tests, passed
    tests += 1
    if condition:
        passed += 1
    else:
        print(f"FAILED: {name}", file=sys.stderr)


record = records.parse("  alpha=7 ")
check(record == ("alpha", 7), "parses trimmed record")
check(records.parse("broken") is None, "rejects malformed input")
check(records.parse("=3") is None, "rejects empty key")

totals = aggregate.aggregate(["alpha=7", "beta=2", "alpha=5", "ignored", "beta=3", "=1"])
check(totals == {"alpha": 12, "beta": 5}, "aggregates valid lines")
check(len(totals) == 2, "ignores malformed lines")

print(f'{{"tests": {tests}, "passed": {passed}}}')
raise SystemExit(0 if passed == tests else 1)
