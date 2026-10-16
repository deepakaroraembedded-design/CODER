#include "cf/gates/profile.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace cf::gates {
namespace {

struct ProfileError : std::runtime_error {
  ProfileError(const char* code_in, const std::string& message)
      : std::runtime_error(message), code(code_in) {}
  const char* code;
};

[[noreturn]] void schema_error(const std::string& message) {
  throw ProfileError(error_codes::kProfileSchemaInvalid, message);
}

std::string as_string(const YAML::Node& node, const std::string& context) {
  if (!node.IsScalar()) {
    schema_error(context + " must be a scalar");
  }
  try {
    return node.as<std::string>();
  } catch (const YAML::Exception& e) {
    schema_error(context + ": " + e.what());
  }
}

std::string required_string(const YAML::Node& map, const char* key,
                            const std::string& context) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    schema_error(context + " is missing required field '" + key + "'");
  }
  std::string value = as_string(node, context + "." + key);
  if (value.empty()) {
    schema_error(context + "." + key + " must not be empty");
  }
  return value;
}

std::vector<std::string> string_list(const YAML::Node& node,
                                     const std::string& context) {
  std::vector<std::string> out;
  if (!node || node.IsNull()) {
    return out;
  }
  if (!node.IsSequence()) {
    schema_error(context + " must be a sequence");
  }
  for (std::size_t i = 0; i < node.size(); ++i) {
    out.push_back(as_string(node[i], context + "[" + std::to_string(i) + "]"));
  }
  return out;
}

double optional_double(const YAML::Node& map, const char* key, double fallback) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    return fallback;
  }
  if (!node.IsScalar()) {
    schema_error(std::string(key) + " must be a number");
  }
  try {
    return node.as<double>();
  } catch (const YAML::Exception& e) {
    schema_error(std::string(key) + ": " + e.what());
  }
}

GateStep parse_step(const YAML::Node& node, const std::string& context) {
  if (!node.IsSequence() || node.size() == 0) {
    schema_error(context + " must be a non-empty argument list");
  }
  GateStep step;
  for (std::size_t i = 0; i < node.size(); ++i) {
    std::string element = as_string(node[i], context + "[" + std::to_string(i) + "]");
    if (element.empty()) {
      schema_error(context + " contains an empty argument");
    }
    step.argv.push_back(std::move(element));
  }
  return step;
}

GateSpec parse_gate_spec(const YAML::Node& node, const std::string& context) {
  GateSpec spec;
  if (!node || node.IsNull()) {
    return spec;
  }
  if (!node.IsMap()) {
    schema_error(context + " must be a mapping");
  }
  const YAML::Node steps = node["steps"];
  if (steps && !steps.IsNull()) {
    if (!steps.IsSequence()) {
      schema_error(context + ".steps must be a sequence");
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
      spec.steps.push_back(
          parse_step(steps[i], context + ".steps[" + std::to_string(i) + "]"));
    }
  }
  return spec;
}

Profile parse_profile(const YAML::Node& root) {
  if (!root || !root.IsMap()) {
    schema_error("profile must be a YAML mapping");
  }
  Profile profile;
  profile.id = required_string(root, "id", "profile");
  profile.image = required_string(root, "image", "profile");
  if (const YAML::Node backend = root["codeintel_backend"]) {
    profile.codeintel_backend = as_string(backend, "profile.codeintel_backend");
  }

  const YAML::Node languages = root["languages"];
  if (!languages || languages.IsNull() || !languages.IsMap() || languages.size() == 0) {
    schema_error("profile must declare at least one language");
  }
  for (const auto& entry : languages) {
    const std::string language = as_string(entry.first, "languages.<name>");
    const YAML::Node node = entry.second;
    if (!node.IsMap()) {
      schema_error("languages." + language + " must be a mapping");
    }
    LanguageSpec spec;
    spec.standard_var = required_string(node, "standard_var", "languages." + language);
    spec.guardrails = parse_gate_spec(node["guardrails"], "languages." + language + ".guardrails");
    spec.format = parse_gate_spec(node["format"], "languages." + language + ".format");
    profile.languages.emplace(language, std::move(spec));
  }

  if (const YAML::Node build_systems = root["build_systems"]) {
    if (!build_systems.IsMap()) {
      schema_error("profile.build_systems must be a mapping");
    }
    for (const auto& entry : build_systems) {
      const std::string name = as_string(entry.first, "build_systems.<name>");
      const YAML::Node node = entry.second;
      if (!node.IsMap()) {
        schema_error("build_systems." + name + " must be a mapping");
      }
      BuildSystemSpec spec;
      const YAML::Node steps = node["steps"];
      if (steps && !steps.IsNull()) {
        if (!steps.IsSequence()) {
          schema_error("build_systems." + name + ".steps must be a sequence");
        }
        for (std::size_t i = 0; i < steps.size(); ++i) {
          spec.steps.push_back(parse_step(
              steps[i], "build_systems." + name + ".steps[" + std::to_string(i) + "]"));
        }
      }
      spec.agent_commands =
          string_list(node["agent_commands"], "build_systems." + name + ".agent_commands");
      profile.build_systems.emplace(name, std::move(spec));
    }
  }

  if (const YAML::Node frameworks = root["test_frameworks"]) {
    if (!frameworks.IsMap()) {
      schema_error("profile.test_frameworks must be a mapping");
    }
    for (const auto& entry : frameworks) {
      const std::string name = as_string(entry.first, "test_frameworks.<name>");
      const YAML::Node node = entry.second;
      if (!node.IsMap()) {
        schema_error("test_frameworks." + name + " must be a mapping");
      }
      TestFrameworkSpec spec;
      const YAML::Node steps = node["steps"];
      if (steps && !steps.IsNull()) {
        if (!steps.IsSequence()) {
          schema_error("test_frameworks." + name + ".steps must be a sequence");
        }
        for (std::size_t i = 0; i < steps.size(); ++i) {
          spec.steps.push_back(parse_step(
              steps[i], "test_frameworks." + name + ".steps[" + std::to_string(i) + "]"));
        }
      }
      if (const YAML::Node report = node["report"]) {
        spec.report = as_string(report, "test_frameworks." + name + ".report");
      }
      spec.languages =
          string_list(node["languages"], "test_frameworks." + name + ".languages");
      profile.test_frameworks.emplace(name, std::move(spec));
    }
  }

  if (const YAML::Node common = root["common"]) {
    if (!common.IsMap()) {
      schema_error("profile.common must be a mapping");
    }
    for (const auto& entry : common) {
      const std::string name = as_string(entry.first, "common.<name>");
      const YAML::Node node = entry.second;
      if (name == "runtime") {
        profile.runtime.presets = string_list(node["presets"], "common.runtime.presets");
        if (const YAML::Node tsan = node["tsan_when"]) {
          profile.runtime.tsan_when = as_string(tsan, "common.runtime.tsan_when");
        }
      } else if (name == "coverage") {
        if (const YAML::Node tool = node["tool"]) {
          profile.coverage.tool = as_string(tool, "common.coverage.tool");
        }
        profile.coverage.line = optional_double(node, "line", 0.0);
        profile.coverage.branch = optional_double(node, "branch", 0.0);
      } else if (name == "mutation") {
        if (const YAML::Node tool = node["tool"]) {
          profile.mutation.tool = as_string(tool, "common.mutation.tool");
        }
        profile.mutation.min_score = optional_double(node, "min_score", 0.0);
      } else {
        profile.common.emplace(name, parse_gate_spec(node, "common." + name));
      }
    }
  }

  profile.exec_from_source =
      string_list(root["exec_from_source"], "profile.exec_from_source");
  for (const auto& path : profile.exec_from_source) {
    if (path.find('*') != std::string::npos) {
      schema_error("exec_from_source entries must be exact paths, got '" + path + "'");
    }
  }
  return profile;
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

}  // namespace

Result<Profile> load_profile_string(std::string_view text) {
  try {
    YAML::Node root;
    try {
      root = YAML::Load(std::string(text));
    } catch (const YAML::Exception& e) {
      throw ProfileError(error_codes::kProfileParseFailed,
                         "cannot parse profile YAML: " + std::string(e.what()));
    }
    return parse_profile(root);
  } catch (const ProfileError& e) {
    return fail(e.code, e.what());
  } catch (const std::exception& e) {
    return fail(error_codes::kProfileParseFailed, e.what());
  }
}

Result<Profile> load_profile_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kProfileParseFailed,
                "cannot open profile file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return load_profile_string(buffer.str());
}

Status ProfileRegistry::add(Profile profile) {
  const std::string id = profile.id;
  if (profiles_.find(id) != profiles_.end()) {
    return fail(error_codes::kProfileDuplicate, "duplicate profile id '" + id + "'");
  }
  profiles_.emplace(id, std::move(profile));
  return {};
}

Result<const Profile*> ProfileRegistry::find(std::string_view id) const {
  const auto it = profiles_.find(id);
  if (it == profiles_.end()) {
    return fail(error_codes::kProfileNotFound, "no profile with id '" + std::string(id) + "'");
  }
  return &it->second;
}

Result<ProfileRegistry> ProfileRegistry::load_dir(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return fail(error_codes::kProfileNotFound,
                "profiles directory not found: " + dir.string());
  }
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string extension = entry.path().extension().string();
    if (extension == ".yaml" || extension == ".yml") {
      files.push_back(entry.path());
    }
  }
  if (ec) {
    return fail(error_codes::kProfileNotFound,
                "cannot list profiles directory: " + dir.string());
  }
  std::sort(files.begin(), files.end());
  ProfileRegistry registry;
  for (const auto& file : files) {
    auto profile = load_profile_file(file);
    if (!profile) {
      return tl::make_unexpected(profile.error());
    }
    auto added = registry.add(std::move(*profile));
    if (!added) {
      return tl::make_unexpected(added.error());
    }
  }
  return registry;
}

Result<const Profile*> ProfileRegistry::resolve(std::string_view language,
                                                std::string_view build_system,
                                                std::string_view test_framework) const {
  for (const auto& [id, profile] : profiles_) {
    if (profile.languages.find(language) == profile.languages.end()) {
      continue;
    }
    if (profile.build_systems.find(build_system) == profile.build_systems.end()) {
      continue;
    }
    const auto framework = profile.test_frameworks.find(test_framework);
    if (framework == profile.test_frameworks.end()) {
      continue;
    }
    if (!framework->second.languages.empty() &&
        std::find(framework->second.languages.begin(), framework->second.languages.end(),
                  language) == framework->second.languages.end()) {
      continue;
    }
    return &profile;
  }
  return fail(error_codes::kNoMatchingToolchain,
              "no toolchain profile declares " + std::string(language) + "/" +
                  std::string(build_system) + "/" + std::string(test_framework));
}

const std::map<std::string, Profile, std::less<>>& ProfileRegistry::all() const {
  return profiles_;
}

Result<std::vector<std::string>> expand_step(const GateStep& step,
                                             const StepContext& context) {
  const std::vector<std::pair<std::string, std::string>> placeholders = {
      {"{src_dir}", context.src_dir},
      {"{build_dir}", context.build_dir},
      {"{standard}", context.standard},
      {"{standard_number}", context.standard_number},
      {"{standard_var}", context.standard_var},
  };
  std::vector<std::string> out;
  for (const auto& element : step.argv) {
    if (element == "{changed_files...}") {
      out.insert(out.end(), context.changed_files.begin(), context.changed_files.end());
      continue;
    }
    std::string expanded = element;
    for (const auto& [name, value] : placeholders) {
      replace_all(expanded, name, value);
    }
    const std::size_t open = expanded.find('{');
    if (open != std::string::npos) {
      return fail(error_codes::kUnknownPlaceholder,
                  "unknown placeholder in argument '" + element + "'");
    }
    out.push_back(std::move(expanded));
  }
  return out;
}

std::string standard_number(std::string_view standard) {
  std::size_t end = standard.size();
  while (end > 0 && !std::isdigit(static_cast<unsigned char>(standard[end - 1]))) {
    --end;
  }
  std::size_t begin = end;
  while (begin > 0 && std::isdigit(static_cast<unsigned char>(standard[begin - 1]))) {
    --begin;
  }
  if (begin == end) {
    return {};
  }
  return std::string(standard.substr(begin, end - begin));
}

}  // namespace cf::gates
