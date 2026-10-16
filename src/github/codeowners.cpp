#include "cf/github/codeowners.hpp"

#include <algorithm>
#include <sstream>

namespace cf::github {
namespace {

bool push_unique(std::vector<std::string>& values, std::string value) {
  if (value.empty()) {
    return false;
  }
  if (std::find(values.begin(), values.end(), value) != values.end()) {
    return false;
  }
  values.push_back(std::move(value));
  return true;
}

}  // namespace

Codeowners parse_codeowners(std::string_view text) {
  Codeowners owners;
  std::istringstream lines{std::string(text)};
  std::string line;
  while (std::getline(lines, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    std::istringstream tokens(line);
    std::string token;
    bool first = true;  // the first token is the path pattern
    while (tokens >> token) {
      if (first) {
        first = false;
        continue;
      }
      if (token.size() < 2 || token[0] != '@') {
        continue;  // section headers and stray text are not owners
      }
      const std::string owner = token.substr(1);
      const auto slash = owner.find('/');
      if (slash == std::string::npos) {
        push_unique(owners.users, owner);
      } else {
        push_unique(owners.teams, owner.substr(slash + 1));
      }
    }
  }
  return owners;
}

}  // namespace cf::github
