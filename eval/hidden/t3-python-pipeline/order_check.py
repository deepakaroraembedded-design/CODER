import os
import sys


sys.path.insert(0, sys.argv[1])
from models import LineItem, Money
from processor import process


def _cf_write_hidden_result(tests, passed):
    path = os.environ.get("CF_HIDDEN_RESULT")
    if path:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('{"tests": %d, "passed": %d}\n' % (tests, passed))

tests = 0
passed = 0


def check(condition, name):
    global tests, passed
    tests += 1
    if condition:
        passed += 1
    else:
        print(f"FAILED: {name}", file=sys.stderr)


ledger, total = process([])
check(ledger.item_count() == 0 and total.cents == 0, "empty order")
ledger, total = process([
    LineItem("A", 2, Money(500)),
    LineItem("B", 1, Money(250)),
    LineItem("A", 1, Money(100)),
])
check(ledger.item_count() == 3, "duplicate SKUs distinct")
check(total.cents == 1350, "total")
check(len(ledger.items) == 3, "items exposed")
print(f'{{"tests": {tests}, "passed": {passed}}}')
raise SystemExit(0 if passed == tests else 1)
