// cf-orch: versioned role prompt templates (design section 6.6).
//
// Prompts are treated as code: `prompts/<role>.md` holds the reviewable
// template, `{{name}}` marks a placeholder, and embedded defaults keep the
// binary working without the directory. Unknown placeholders are rejected at
// load time; lines whose value is empty are dropped at render time.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::orch {

namespace error_codes {
inline constexpr const char* kPromptNotFound = "ORCH_PROMPT_NOT_FOUND";
inline constexpr const char* kPromptInvalid = "ORCH_PROMPT_INVALID";
}  // namespace error_codes

class PromptLibrary {
 public:
  static PromptLibrary defaults();

  // Loads `*.md` from dir, overriding the embedded defaults per role.
  static Result<PromptLibrary> load_dir(const std::filesystem::path& dir);

  bool has(std::string_view role) const;

  std::vector<std::string> roles() const;

  // Replaces every {{name}}; missing values become empty and their line is
  // dropped when nothing but the label remains.
  Result<std::string> render(
      std::string_view role,
      const std::map<std::string, std::string, std::less<>>& values) const;

 private:
  std::map<std::string, std::string, std::less<>> templates_;
};

}  // namespace cf::orch
