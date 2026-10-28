#include <cmath>
#include <iostream>

#include "golden/matrix.hpp"

int main() {
  const golden::Matrix2 identity{1, 0, 0, 1};
  const golden::Matrix2 value{1, 2, 3, 4};
  const auto product = golden::multiply(identity, value);
  const auto transposed = golden::transpose(value);
  int tests = 0;
  int passed = 0;
  auto check = [&](bool condition, const char* name) {
    ++tests;
    if (condition) {
      ++passed;
    } else {
      std::cerr << "FAILED: " << name << "\n";
    }
  };
  check(product.a == 1 && product.b == 2 && product.c == 3 && product.d == 4,
        "identity multiply");
  check(transposed.a == 1 && transposed.b == 3 && transposed.c == 2 &&
            transposed.d == 4,
        "transpose");
  check(golden::determinant(value) == -2, "determinant");
  std::cout << "{\"tests\":" << tests << ",\"passed\":" << passed << "}\n";
  return passed == tests ? 0 : 1;
}
