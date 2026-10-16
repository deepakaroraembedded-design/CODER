#include "cf/github/secrets.hpp"

#include <regex>

namespace cf::github {
namespace {

struct Pattern {
  const char* name;
  const std::regex* regex;
};

const std::regex kAwsAccessKey(R"(AKIA[0-9A-Z]{16})");
const std::regex kGithubToken(R"(gh[pousr]_[A-Za-z0-9]{36,})");
const std::regex kPrivateKey(
    R"(-----BEGIN (RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----)");
const std::regex kSlackToken(R"(xox[baprs]-[A-Za-z0-9-]{10,})");
const std::regex kGenericSecret(
    R"((password|passwd|api[_-]?key|secret|token)\s*[:=]\s*["'][^"']{8,}["'])",
    std::regex::icase);

const Pattern kPatterns[] = {
    {"aws-access-key", &kAwsAccessKey},
    {"github-token", &kGithubToken},
    {"private-key", &kPrivateKey},
    {"slack-token", &kSlackToken},
    {"generic-secret", &kGenericSecret},
};

}  // namespace

std::vector<SecretFinding> scan_for_secrets(std::string_view text) {
  std::vector<SecretFinding> findings;
  int line_number = 1;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    std::size_t end = text.find('\n', begin);
    const std::string_view line =
        end == std::string_view::npos ? text.substr(begin)
                                      : text.substr(begin, end - begin);
    for (const auto& pattern : kPatterns) {
      if (std::regex_search(line.begin(), line.end(), *pattern.regex)) {
        findings.push_back({pattern.name, line_number});
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
    ++line_number;
  }
  return findings;
}

}  // namespace cf::github
