#include "cf/rag/injection_scan.hpp"

#include <algorithm>
#include <cctype>

namespace cf::rag {
namespace {

struct Rule {
  const char* name;
  const char* needle;  // matched case-insensitively
};

constexpr Rule kRules[] = {
    {"ignore-previous", "ignore previous instructions"},
    {"ignore-previous", "ignore all previous"},
    {"ignore-previous", "ignore the above"},
    {"ignore-previous", "ignore the previous"},
    {"disregard", "disregard previous"},
    {"disregard", "disregard prior"},
    {"disregard", "disregard the above"},
    {"you-must-also", "you must also"},
    {"do-not-tell", "do not tell the user"},
    {"do-not-tell", "don't tell the user"},
    {"system-prompt", "system prompt"},
    {"as-an-ai", "as an ai language model"},
    {"new-instructions", "new instructions"},
    {"override", "override your instructions"},
    {"override", "override the instructions"},
};

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string trim_snippet(std::string_view line) {
  std::size_t begin = 0;
  while (begin < line.size() &&
         std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
    ++begin;
  }
  std::size_t end = line.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
    --end;
  }
  std::string snippet(line.substr(begin, end - begin));
  if (snippet.size() > 200) {
    snippet.resize(200);
    snippet += "...";
  }
  return snippet;
}

}  // namespace

std::vector<InjectionFinding> scan_for_injected_instructions(std::string_view text) {
  const std::string lowered = to_lower(text);
  std::vector<InjectionFinding> findings;
  for (const auto& rule : kRules) {
    std::size_t pos = 0;
    while ((pos = lowered.find(rule.needle, pos)) != std::string::npos) {
      // Compute the 1-based line number and the line's text.
      const int line =
          static_cast<int>(std::count(lowered.begin(), lowered.begin() +
                                                          static_cast<std::ptrdiff_t>(pos),
                                      '\n')) +
          1;
      const std::size_t line_begin = lowered.rfind('\n', pos);
      const std::size_t line_end = lowered.find('\n', pos);
      const std::string_view snippet =
          text.substr(line_begin == std::string::npos ? 0 : line_begin + 1,
                      (line_end == std::string::npos ? text.size() : line_end) -
                          (line_begin == std::string::npos ? 0 : line_begin + 1));
      InjectionFinding finding;
      finding.pattern = rule.name;
      finding.line = line;
      finding.snippet = trim_snippet(snippet);
      findings.push_back(std::move(finding));
      pos += std::string_view(rule.needle).size();
    }
  }
  std::sort(findings.begin(), findings.end(),
            [](const InjectionFinding& a, const InjectionFinding& b) {
              if (a.line != b.line) {
                return a.line < b.line;
              }
              return a.pattern < b.pattern;
            });
  findings.erase(std::unique(findings.begin(), findings.end(),
                             [](const InjectionFinding& a, const InjectionFinding& b) {
                               return a.line == b.line && a.pattern == b.pattern;
                             }),
                 findings.end());
  return findings;
}

}  // namespace cf::rag
