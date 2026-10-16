// cf-github: CODEOWNERS parsing (design section 10.4.4).
//
// GitHub's own rules are the source of truth for the merge; CF only requests
// the owners on the ready pull request so the right humans are asked. The
// parser collects every owner mentioned in the file: `@user` entries become
// reviewer users and `@org/team` entries become team reviewers (team slug).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cf::github {

struct Codeowners {
  std::vector<std::string> users;  // @user
  std::vector<std::string> teams;  // @org/team -> "team"
};

Codeowners parse_codeowners(std::string_view text);

}  // namespace cf::github
