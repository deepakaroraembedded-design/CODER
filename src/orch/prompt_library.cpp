#include "cf/orch/prompt_library.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace cf::orch {
namespace {

constexpr const char* kPlaceholders[] = {"run_id",      "milestone_id", "language",
                                         "standard",    "requirements", "open_defects",
                                         "brief",       "submission_tools",
                                         "commands"};

const char* kCoderDefault = R"prompt(You are the Coder agent of the Coder Framework. Implement exactly the task, follow the company guidelines and the approved design, and keep the change inside the milestone's declared write set.

Run: {{run_id}}
Milestone: {{milestone_id}}
Target language: {{language}} ({{standard}})
Requirements: {{requirements}}
Open defects: {{open_defects}}

Task: {{brief}}

Tools: inspect the worktree with the read, glob and grep tools. Shell access is limited to single allowed commands ({{commands}}); never chain commands, pipe them or run scripts. If a tool call is denied or fails, do not stop: adapt and continue. The tests under tests/ are QA-owned: never edit them; if one cannot compile or contradicts the interface, end the turn with dispute_test instead.

Do not ask questions. Finish by calling the submission tool ({{submission_tools}}).
)prompt";

const char* kReviewerDefault = R"prompt(You are the Reviewer agent of the Coder Framework. Judge the change against the design, contracts and guardrails. Only blocker and major findings block the milestone.

Run: {{run_id}}
Milestone: {{milestone_id}}
Target language: {{language}} ({{standard}})
Requirements: {{requirements}}
Open defects: {{open_defects}}

Task: {{brief}}

Tools: review from the diff; shell access is limited to single read-only commands ({{commands}}). If a tool call is denied or fails, do not stop: adapt and continue.

Do not ask questions. Finish by calling the submission tool ({{submission_tools}}).
)prompt";

const char* kQaDefault = R"prompt(You are the QA agent of the Coder Framework. Tests come from the requirements and interface contracts, never from the implementation. File a structured defect for every failure.

Run: {{run_id}}
Milestone: {{milestone_id}}
Target language: {{language}} ({{standard}})
Requirements: {{requirements}}
Open defects: {{open_defects}}

Task: {{brief}}

Tools: inspect the worktree with the read, glob and grep tools; author the tests with the write/edit tools under tests/ (the repository already carries the build skeleton). Shell access is limited to single allowed commands ({{commands}}); never chain commands, pipe them or run scripts. If a tool call is denied or fails, do not stop: adapt and continue.

Do not ask questions. Finish by calling the submission tool ({{submission_tools}}).
)prompt";

// Validates that every {{...}} is a known placeholder.
Result<std::string> validate_template(const std::string& text,
                                      const std::string& context) {
  static const std::set<std::string> kKnown(std::begin(kPlaceholders),
                                            std::end(kPlaceholders));
  std::size_t pos = 0;
  while ((pos = text.find("{{", pos)) != std::string::npos) {
    const std::size_t end = text.find("}}", pos + 2);
    if (end == std::string::npos) {
      return fail(error_codes::kPromptInvalid,
                  context + ": unterminated placeholder at byte " +
                      std::to_string(pos));
    }
    std::string name = text.substr(pos + 2, end - pos - 2);
    const std::size_t begin = name.find_first_not_of(" \t");
    const std::size_t last = name.find_last_not_of(" \t");
    name = begin == std::string::npos ? std::string()
                                      : name.substr(begin, last - begin + 1);
    if (kKnown.count(name) == 0) {
      return fail(error_codes::kPromptInvalid,
                  context + ": unknown placeholder '{{" + name + "}}'");
    }
    pos = end + 2;
  }
  return text;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// Drops lines that contain only a label and no value ("Requirements: ").
std::string drop_empty_label_lines(const std::string& text) {
  std::istringstream stream(text);
  std::string line;
  std::string out;
  while (std::getline(stream, line)) {
    std::size_t end = line.find_last_not_of(" \t");
    if (end != std::string::npos && line[end] == ':') {
      continue;
    }
    out += line;
    out.push_back('\n');
  }
  return out;
}

}  // namespace

PromptLibrary PromptLibrary::defaults() {
  PromptLibrary library;
  library.templates_["coder"] = kCoderDefault;
  library.templates_["reviewer"] = kReviewerDefault;
  library.templates_["qa"] = kQaDefault;
  return library;
}

Result<PromptLibrary> PromptLibrary::load_dir(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return fail(error_codes::kPromptNotFound,
                "prompts directory not found: " + dir.string());
  }
  PromptLibrary library = defaults();
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".md") {
      continue;
    }
    const std::string role = entry.path().stem().string();
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) {
      return fail(error_codes::kPromptNotFound,
                  "cannot read prompt template: " + entry.path().string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto validated = validate_template(buffer.str(), entry.path().string());
    if (!validated) {
      return tl::make_unexpected(validated.error());
    }
    library.templates_[role] = std::move(*validated);
  }
  return library;
}

bool PromptLibrary::has(std::string_view role) const {
  return templates_.find(role) != templates_.end();
}

std::vector<std::string> PromptLibrary::roles() const {
  std::vector<std::string> out;
  out.reserve(templates_.size());
  for (const auto& [role, text] : templates_) {
    (void)text;
    out.push_back(role);
  }
  return out;
}

Result<std::string> PromptLibrary::render(
    std::string_view role,
    const std::map<std::string, std::string, std::less<>>& values) const {
  const auto template_it = templates_.find(role);
  if (template_it == templates_.end()) {
    return fail(error_codes::kPromptNotFound,
                "no prompt template for role '" + std::string(role) + "'");
  }
  std::string text = template_it->second;
  for (const char* placeholder : kPlaceholders) {
    const auto value = values.find(placeholder);
    replace_all(text, std::string("{{") + placeholder + "}}",
                value == values.end() ? std::string() : value->second);
  }
  return drop_empty_label_lines(text);
}

}  // namespace cf::orch
