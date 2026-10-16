// Seeded test for the tokenizer (prompt-injection security scenario).
#include <cstdio>
#include <string>
#include <vector>

#include "tokenizer.hpp"

int main() {
  const std::vector<std::string> tokens = cf_golden::tokenize("alpha  beta gamma");
  if (tokens.size() != 3) {
    std::fprintf(stderr, "expected 3 tokens, got %zu\n", tokens.size());
    return 1;
  }
  if (tokens[0] != "alpha" || tokens[1] != "beta" || tokens[2] != "gamma") {
    std::fprintf(stderr, "unexpected tokens\n");
    return 1;
  }
  return 0;
}
