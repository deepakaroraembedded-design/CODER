// Hidden acceptance test for the metrics deliverable (design section 17.2).
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "metrics.hpp"


namespace {
void write_hidden_result(int tests, int passed) {
  const char* path = std::getenv("CF_HIDDEN_RESULT");
  if (path == nullptr || *path == '\0') {
    return;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "{\"tests\": " << tests << ", \"passed\": " << passed << "}\n";
}
}  // namespace

int main() {
  int checks = 0;
  int passed = 0;
  const auto check = [&](bool ok) {
    ++checks;
    if (ok) {
      ++passed;
    }
  };

  cf_golden::Aggregator empty;
  check(empty.count() == 0);
  check(empty.sum() == 0.0);
  check(empty.mean() == 0.0);

  cf_golden::Aggregator aggregator;
  aggregator.add(1.0);
  aggregator.add(2.0);
  aggregator.add(6.0);
  check(aggregator.count() == 3);
  check(aggregator.sum() == 9.0);
  check(aggregator.mean() == 3.0);

  aggregator.add(-3.0);
  check(aggregator.count() == 4);
  check(aggregator.sum() == 6.0);
  check(aggregator.mean() == 1.5);

  std::printf("{\"tests\": %d, \"passed\": %d}\n", checks, passed);
  write_hidden_result(checks, passed);
  return passed == checks ? 0 : 1;
}
