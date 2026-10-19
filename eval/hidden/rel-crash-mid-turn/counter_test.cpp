// Hidden acceptance test for the counter deliverable (design section 17.2).
#include <cstdio>

#include "counter.hpp"

int main() {
  int checks = 0;
  int passed = 0;
  const auto check = [&](bool ok) {
    ++checks;
    if (ok) {
      ++passed;
    }
  };

  cf_golden::Counter counter;
  check(counter.value() == 0);
  counter.increment();
  counter.increment();
  check(counter.value() == 2);
  counter.reset();
  check(counter.value() == 0);
  counter.increment();
  check(counter.value() == 1);

  std::printf("{\"tests\": %d, \"passed\": %d}\n", checks, passed);
  return passed == checks ? 0 : 1;
}
