// cf-github: secret scanning for pre-push checks (design section 10.4.6
// check 4; the design uses gitleaks — this is the built-in fallback scan).
//
// Findings never carry the secret itself, only the pattern name and the line,
// so logs and issues stay clean (design section 13: matches are redacted).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cf::github {

struct SecretFinding {
  std::string pattern;  // e.g. "aws-access-key"
  int line = 0;         // 1-based
};

std::vector<SecretFinding> scan_for_secrets(std::string_view text);

}  // namespace cf::github
