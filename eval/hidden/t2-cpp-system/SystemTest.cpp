#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "golden/accumulator.hpp"
#include "golden/parser.hpp"

namespace {
int tests = 0;
int passed = 0;

void check(bool condition, const char* name) {
  ++tests;
  if (condition) {
    ++passed;
  } else {
    std::cerr << "FAILED: " << name << "\n";
  }
}
}  // namespace

int main() {
  const auto parsed = golden::parse_record("  alpha=7 ");
  check(parsed.has_value() && parsed->first == "alpha" && parsed->second == 7,
        "parses trimmed record");
  check(!golden::parse_record("broken").has_value(), "rejects malformed line");
  check(!golden::parse_record("=3").has_value(), "rejects empty key");

  const std::map<std::string, int> totals = golden::accumulate(
      {"alpha=7", "beta=2", "alpha=5", "ignored", "beta=3", "=1"});
  check(totals == std::map<std::string, int>{{"alpha", 12}, {"beta", 5}},
        "aggregates valid lines");
  check(totals.size() == 2, "ignores malformed lines");

  std::cout << "{\"tests\":" << tests << ",\"passed\":" << passed << "}\n";
  return passed == tests ? 0 : 1;
}
