#include "cf/github/licenses.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace cf::github {
namespace {

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> vcpkg_dependencies(const std::string& content) {
  std::vector<std::string> out;
  const auto parsed = nlohmann::json::parse(content, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return out;
  }
  const auto dependencies = parsed.find("dependencies");
  if (dependencies == parsed.end() || !dependencies->is_array()) {
    return out;
  }
  for (const auto& entry : *dependencies) {
    if (entry.is_string()) {
      out.push_back(entry.get<std::string>());
    } else if (entry.is_object()) {
      const std::string name = entry.value("name", "");
      if (!name.empty()) {
        out.push_back(name);
      }
    }
  }
  return out;
}

std::vector<std::string> requirements_dependencies(const std::string& content) {
  std::vector<std::string> out;
  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = trim(line);
    if (line.empty() || line[0] == '-' || line[0] == '=') {
      continue;
    }
    // Strip environment markers and version specifiers.
    const std::size_t marker = line.find(';');
    if (marker != std::string::npos) {
      line = trim(line.substr(0, marker));
    }
    const std::size_t specifier = line.find_first_of("<>=!~[");
    if (specifier != std::string::npos) {
      line = trim(line.substr(0, specifier));
    }
    if (!line.empty()) {
      out.push_back(line);
    }
  }
  return out;
}

std::vector<std::string> package_json_dependencies(const std::string& content) {
  std::vector<std::string> out;
  const auto parsed = nlohmann::json::parse(content, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return out;
  }
  for (const char* section : {"dependencies", "devDependencies"}) {
    const auto node = parsed.find(section);
    if (node == parsed.end() || !node->is_object()) {
      continue;
    }
    for (auto it = node->begin(); it != node->end(); ++it) {
      out.push_back(it.key());
    }
  }
  return out;
}

std::vector<std::string> go_mod_dependencies(const std::string& content) {
  std::vector<std::string> out;
  std::istringstream stream(content);
  std::string line;
  bool in_require_block = false;
  while (std::getline(stream, line)) {
    line = trim(line);
    if (line.rfind("require (", 0) == 0) {
      in_require_block = true;
      continue;
    }
    if (in_require_block && line == ")") {
      in_require_block = false;
      continue;
    }
    if (in_require_block || line.rfind("require ", 0) == 0) {
      const std::string rest =
          in_require_block ? line : trim(line.substr(std::string("require ").size()));
      std::istringstream fields(rest);
      std::string module;
      fields >> module;
      if (!module.empty() && module != "(") {
        out.push_back(module);
      }
    }
  }
  return out;
}

std::string base_name(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? std::string(path)
                                         : std::string(path.substr(slash + 1));
}

}  // namespace

Result<LicenseDatabase> LicenseDatabase::load_string(std::string_view yaml) {
  try {
    YAML::Node root;
    try {
      root = YAML::Load(std::string(yaml));
    } catch (const YAML::Exception& e) {
      throw std::runtime_error(std::string("cannot parse the licence database: ") +
                               e.what());
    }
    LicenseDatabase database;
    const YAML::Node licenses = root["licenses"];
    if (!licenses || licenses.IsNull() || !licenses.IsMap()) {
      throw std::runtime_error("the licence database needs a 'licenses' mapping");
    }
    for (const auto& entry : licenses) {
      database.licenses[entry.first.as<std::string>()] = entry.second.as<std::string>();
    }
    return database;
  } catch (const std::exception& e) {
    return fail(error_codes::kLicenseDbInvalid, e.what());
  }
}

Result<LicenseDatabase> LicenseDatabase::load_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kLicenseDbInvalid,
                "cannot open the licence database: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return load_string(buffer.str());
}

std::string LicenseDatabase::lookup(std::string_view package) const {
  const auto it = licenses.find(package);
  return it == licenses.end() ? std::string() : it->second;
}

Result<std::vector<std::string>> dependencies_in_manifest(std::string_view filename,
                                                          std::string_view content) {
  const std::string name = base_name(filename);
  const std::string text(content);
  if (name == "vcpkg.json") {
    return vcpkg_dependencies(text);
  }
  if (name == "requirements.txt") {
    return requirements_dependencies(text);
  }
  if (name == "package.json") {
    return package_json_dependencies(text);
  }
  if (name == "go.mod") {
    return go_mod_dependencies(text);
  }
  return std::vector<std::string>{};
}

std::vector<LicenseFinding> check_licenses(const std::vector<std::string>& dependencies,
                                           const LicenseDatabase& database,
                                           const LicensePolicy& policy) {
  std::set<std::string> allowed;
  for (const auto& entry : policy.allowed) {
    allowed.insert(to_lower(entry));
  }
  std::vector<LicenseFinding> findings;
  for (const auto& dependency : dependencies) {
    const std::string license = database.lookup(dependency);
    if (license.empty()) {
      findings.push_back({dependency, "",
                          "unknown licence for '" + dependency +
                              "' (not in the licence database); needs review"});
      continue;
    }
    if (allowed.count(to_lower(license)) == 0) {
      findings.push_back({dependency, license,
                          "'" + dependency + "' uses " + license +
                              ", which is not on the licence allow-list"});
    }
  }
  return findings;
}

}  // namespace cf::github
