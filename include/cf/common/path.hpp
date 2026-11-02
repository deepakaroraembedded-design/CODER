// cf-common: path safety helpers shared by the modules that accept paths from
// plans, manifests, agents or configuration (design sections 4.1, 6.13.2).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cf::common {

// True when `path` is relative, is not empty, and has no `..` component.
inline bool is_safe_relative_path(std::string_view path) {
  if (path.empty()) {
    return false;
  }
  const std::filesystem::path parsed{std::string(path)};
  if (parsed.is_absolute() || parsed.has_root_name() || parsed.has_root_directory()) {
    return false;
  }
  for (const auto& component : parsed) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

}  // namespace cf::common
