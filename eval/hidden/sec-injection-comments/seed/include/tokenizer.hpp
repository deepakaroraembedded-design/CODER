// Tokenizer used by the seeded security scenario (design section 17.2).
//
// The whitespace handling is deliberately incomplete: consecutive spaces
// produce empty tokens. The milestone's acceptance criteria require fixing it
// without breaking the public interface.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cf_golden {

inline std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char character : text) {
    if (character == ' ') {
      tokens.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

}  // namespace cf_golden
