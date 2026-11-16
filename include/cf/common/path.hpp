// cf-common: path safety helpers shared by the modules that accept paths from
// plans, manifests, agents or configuration (design sections 4.1, 6.13.2).
#pragma once

#include <filesystem>
#include <set>
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

// Build and cache directories an agent may create while working; they are
// never committed by the snapshot (design section 6.13.2). Lockfiles are
// dependency manifests, not build output (B-35).
inline bool is_build_artifact(std::string_view path) {
  static const std::set<std::string_view> ignored = {
      "build",        "out",           "dist",          "target",
      "node_modules", "__pycache__",   ".cache",        ".pytest_cache",
      ".mypy_cache",  ".ruff_cache",   "cmake-build-debug", "cmake-build-release"};
  std::size_t begin = 0;
  while (begin < path.size()) {
    const std::size_t slash = path.find('/', begin);
    const std::string_view component =
        path.substr(begin, slash == std::string_view::npos
                               ? std::string_view::npos
                               : slash - begin);
    if (ignored.count(component) > 0 ||
        component.rfind("cmake-build-", 0) == 0) {
      return true;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    begin = slash + 1;
  }
  return path.size() >= 4 && path.substr(path.size() - 4) == ".pyc";
}

// Heuristic used when a plan declares a top-level write set entry that does
// not exist yet: an extension, a trailing slash or a well-known build/manifest
// name decides whether the sandbox should create a file or a directory.
inline bool looks_like_file(std::string_view path) {
  if (path.empty() || path.back() == '/') {
    return false;
  }
  const std::size_t slash = path.find_last_of('/');
  const std::string_view name =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (name.empty()) {
    return false;
  }
  if (name.find('.') != std::string_view::npos) {
    return true;
  }
  static constexpr std::string_view kFiles[] = {
      "Makefile",      "makefile",     "GNUmakefile",  "CMakeLists.txt",
      "meson.build",   "meson_options.txt", "BUILD",   "BUILD.bazel",
      "WORKSPACE",     "WORKSPACE.bazel", "pom.xml",  "build.gradle",
      "build.gradle.kts", "settings.gradle", "settings.gradle.kts",
      "go.mod",        "go.sum",       "Cargo.toml",  "Gemfile",
      "Rakefile",      "Dockerfile",   "Vagrantfile"};
  for (const auto candidate : kFiles) {
    if (name == candidate) {
      return true;
    }
  }
  return false;
}

}  // namespace cf::common
