// cf-rag: injected-instruction scan for mirrored content (design section
// 6.3.1 step 4).
//
// Text that addresses an AI or gives it commands is flagged; the set is held
// for human review and is never indexed automatically.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cf::rag {

struct InjectionFinding {
  std::string pattern;  // the rule that matched
  int line = 0;         // 1-based line number
  std::string snippet;  // the matching line, trimmed and capped
};

std::vector<InjectionFinding> scan_for_injected_instructions(std::string_view text);

}  // namespace cf::rag
