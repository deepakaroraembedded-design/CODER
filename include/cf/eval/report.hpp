// cf-eval: report rendering (design section 17.2): machine-readable JSON for
// CI and a Markdown summary for humans.
#pragma once

#include <string>

#include "cf/eval/score.hpp"

namespace cf::eval {

std::string score_to_json(const SuiteScore& score);
std::string score_to_markdown(const SuiteScore& score);

}  // namespace cf::eval
