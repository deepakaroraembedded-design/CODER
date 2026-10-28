#include "cf/refmirror/versions.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace cf::refmirror {
namespace {

using Json = nlohmann::json;

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  return text.substr(begin);
}

// Only an exact version is usable: a range or a caret/tilde comparator would
// make "which version was approved" ambiguous.
std::string exact_version(std::string version) {
  version = trim(std::move(version));
  while (!version.empty() && (version.front() == '^' || version.front() == '~' ||
                              version.front() == '=' || version.front() == 'v')) {
    version.erase(version.begin());
  }
  if (version.empty() || !std::isdigit(static_cast<unsigned char>(version.front()))) {
    return {};
  }
  for (const char c : version) {
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-' &&
        c != '+' && !std::isalpha(static_cast<unsigned char>(c))) {
      return {};  // a range, wildcard or comparator
    }
  }
  return version;
}

void add(std::map<std::string, std::string, std::less<>>& out,
         const std::string& name, const std::string& version) {
  const std::string clean_name = lower(trim(name));
  const std::string clean_version = exact_version(version);
  if (!clean_name.empty() && !clean_version.empty()) {
    out[clean_name] = clean_version;
  }
}

void read_vcpkg(const std::filesystem::path& path,
                std::map<std::string, std::string, std::less<>>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return;
  }
  const Json root = Json::parse(in, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return;
  }
  const auto dependencies = root.find("dependencies");
  if (dependencies == root.end() || !dependencies->is_array()) {
    return;
  }
  for (const auto& dependency : *dependencies) {
    if (dependency.is_string()) {
      continue;  // no explicit version: the baseline decides, keep mirrored
    }
    if (!dependency.is_object()) {
      continue;
    }
    std::string version = dependency.value("version", "");
    if (version.empty()) {
      version = dependency.value("version>=", "");
    }
    if (version.empty()) {
      version = dependency.value("version-semver", "");
    }
    add(out, dependency.value("name", ""), version);
  }
}

void read_requirements(const std::filesystem::path& path,
                       std::map<std::string, std::string, std::less<>>& out) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = trim(std::move(line));
    const std::size_t equals = line.find("==");
    if (equals == std::string::npos) {
      continue;
    }
    std::string name = trim(line.substr(0, equals));
    const std::size_t extra = name.find('[');
    if (extra != std::string::npos) {
      name = name.substr(0, extra);
    }
    std::string version = trim(line.substr(equals + 2));
    const std::size_t marker = version.find(';');
    if (marker != std::string::npos) {
      version = trim(version.substr(0, marker));
    }
    add(out, name, version);
  }
}

void read_package_json(const std::filesystem::path& path,
                       std::map<std::string, std::string, std::less<>>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return;
  }
  const Json root = Json::parse(in, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return;
  }
  for (const char* section : {"dependencies", "devDependencies"}) {
    const auto dependencies = root.find(section);
    if (dependencies == root.end() || !dependencies->is_object()) {
      continue;
    }
    for (const auto& [name, version] : dependencies->items()) {
      if (version.is_string()) {
        add(out, name, version.get<std::string>());
      }
    }
  }
}

}  // namespace

Result<std::map<std::string, std::string, std::less<>>> dependency_versions(
    const std::filesystem::path& project_dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(project_dir, ec)) {
    return fail("REFMIRROR_PROJECT_NOT_FOUND",
                "project directory not found: " + project_dir.string());
  }
  std::map<std::string, std::string, std::less<>> versions;
  read_vcpkg(project_dir / "vcpkg.json", versions);
  read_requirements(project_dir / "requirements.txt", versions);
  read_package_json(project_dir / "package.json", versions);
  return versions;
}

Manifest match_versions(const Manifest& manifest,
                        const std::map<std::string, std::string, std::less<>>& deps,
                        VersionReport& report) {
  Manifest matched;
  for (const auto& set : manifest.sets) {
    const auto dependency = deps.find(lower(set.id));
    if (dependency != deps.end() && !dependency->second.empty() &&
        dependency->second != set.version) {
      report.skipped.push_back(
          SkippedSet{set.id, set.version, dependency->second});
      continue;
    }
    report.pinned[set.id] =
        dependency != deps.end() && !dependency->second.empty()
            ? dependency->second
            : set.version;
    matched.sets.push_back(set);
  }
  return matched;
}

}  // namespace cf::refmirror
