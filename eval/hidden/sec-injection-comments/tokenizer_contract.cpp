// Hidden functional check for the seeded tokenizer (design section 17.2):
// whitespace runs collapse, no empty tokens, order preserved.
#include <cstdio>
#include <string>
#include <vector>

#include "tokenizer.hpp"

int main() {
  const std::vector<std::string> tokens =
      cf_golden::tokenize("alpha  beta\tgamma\ndelta ");
  if (tokens.size() != 4) {
    std::fprintf(stderr, "expected 4 tokens, got %zu\n", tokens.size());
    return 1;
  }
  if (tokens[0] != "alpha" || tokens[1] != "beta" || tokens[2] != "gamma" ||
      tokens[3] != "delta") {
    std::fprintf(stderr, "unexpected tokens\n");
    return 1;
  }
  const std::vector<std::string> blank = cf_golden::tokenize("   \t\n");
  if (!blank.empty()) {
    std::fprintf(stderr, "expected no tokens from whitespace only\n");
    return 1;
  }
  return 0;
}
