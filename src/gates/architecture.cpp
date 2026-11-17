#include "cf/gates/architecture.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

#include "cf/common/path.hpp"

namespace cf::gates {
namespace {

namespace fs = std::filesystem;

bool looks_textual(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  char buffer[4096];
  in.read(buffer, sizeof(buffer));
  const std::streamsize got = in.gcount();
  for (std::streamsize i = 0; i < got; ++i) {
    if (buffer[i] == '\0') {
      return false;
    }
  }
  return true;
}

bool is_import_line(std::string_view line) {
  return line.find("#include") != std::string_view::npos ||
         line.find("import ") != std::string_view::npos ||
         line.find("require(") != std::string_view::npos ||
         line.find(" from \"") != std::string_view::npos ||
         line.find(" from '") != std::string_view::npos;
}

}  // namespace

Result<std::vector<GateFinding>> check_architecture_rules(
    const std::filesystem::path& worktree,
    const std::vector<ArchitectureRule>& rules) {
  std::vector<GateFinding> findings;
  if (rules.empty()) {
    return findings;
  }
  std::error_code ec;
  if (!fs::is_directory(worktree, ec)) {
    return fail("GATE_ARCHITECTURE", "worktree does not exist: " +
                                         worktree.string());
  }
  std::set<std::string> seen;
  for (fs::recursive_directory_iterator it(worktree, ec), end; it != end;
       it.increment(ec)) {
    if (ec) {
      break;
    }
    std::error_code type_ec;
    if (!it->is_regular_file(type_ec)) {
      continue;
    }
    const fs::path relative = fs::relative(it->path(), worktree, type_ec);
    if (type_ec) {
      continue;
    }
    const std::string path = relative.generic_string();
    if (cf::common::is_build_artifact(path) ||
        path.rfind(".git/", 0) == 0) {
      continue;
    }
    bool matches = false;
    for (const auto& rule : rules) {
      if (path.rfind(rule.from_prefix, 0) == 0) {
        matches = true;
        break;
      }
    }
    if (!matches) {
      continue;
    }
    std::error_code size_ec;
    if (fs::file_size(it->path(), size_ec) > 2 * 1024 * 1024 || size_ec ||
        !looks_textual(it->path())) {
      continue;
    }
    std::ifstream in(it->path(), std::ios::binary);
    std::string line;
    std::uint32_t line_number = 0;
    while (std::getline(in, line)) {
      ++line_number;
      if (!is_import_line(line)) {
        continue;
      }
      for (const auto& rule : rules) {
        if (path.rfind(rule.from_prefix, 0) != 0 ||
            line.find(rule.forbidden_prefix) == std::string::npos) {
          continue;
        }
        const std::string message =
            rule.id + ": " + path + ":" + std::to_string(line_number) +
            " references " + rule.forbidden_prefix + " (forbidden for " +
            rule.from_prefix + ")";
        if (!seen.insert(message).second) {
          continue;
        }
        findings.push_back(
            GateFinding{"architecture", "error", message});
      }
    }
  }
  return findings;
}

}  // namespace cf::gates
