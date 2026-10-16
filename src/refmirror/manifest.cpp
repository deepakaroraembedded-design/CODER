#include "cf/refmirror/manifest.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace cf::refmirror {
namespace {

struct ManifestError : std::runtime_error {
  explicit ManifestError(const std::string& message) : std::runtime_error(message) {}
};

std::string require_string(const YAML::Node& map, const char* key,
                           const std::string& context) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    throw ManifestError(context + " is missing required field '" + key + "'");
  }
  if (!node.IsScalar()) {
    throw ManifestError(context + "." + key + " must be a scalar");
  }
  const std::string value = node.as<std::string>();
  if (value.empty()) {
    throw ManifestError(context + "." + key + " must not be empty");
  }
  return value;
}

std::string optional_string(const YAML::Node& map, const char* key) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    return {};
  }
  return node.as<std::string>();
}

std::vector<std::string> string_list(const YAML::Node& node,
                                     const std::string& context) {
  std::vector<std::string> out;
  if (!node || node.IsNull()) {
    return out;
  }
  if (!node.IsSequence()) {
    throw ManifestError(context + " must be a sequence");
  }
  for (const auto& entry : node) {
    out.push_back(entry.as<std::string>());
  }
  return out;
}

bool is_remote(std::string_view source) {
  return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
}

}  // namespace

const ReferenceSet* Manifest::find(std::string_view id) const {
  for (const auto& set : sets) {
    if (set.id == id) {
      return &set;
    }
  }
  return nullptr;
}

Result<Manifest> load_manifest_string(std::string_view text) {
  try {
    YAML::Node root;
    try {
      root = YAML::Load(std::string(text));
    } catch (const YAML::Exception& e) {
      throw ManifestError("cannot parse manifest YAML: " + std::string(e.what()));
    }
    if (!root || !root.IsMap()) {
      throw ManifestError("manifest must be a YAML mapping with a 'sets' list");
    }
    const YAML::Node sets = root["sets"];
    if (!sets || sets.IsNull() || !sets.IsSequence() || sets.size() == 0) {
      throw ManifestError("manifest must declare at least one set");
    }
    Manifest manifest;
    std::set<std::string> ids;
    for (std::size_t i = 0; i < sets.size(); ++i) {
      const YAML::Node node = sets[i];
      const std::string context = "sets[" + std::to_string(i) + "]";
      if (!node.IsMap()) {
        throw ManifestError(context + " must be a mapping");
      }
      ReferenceSet set;
      set.id = require_string(node, "id", context);
      if (!ids.insert(set.id).second) {
        throw ManifestError("duplicate set id '" + set.id + "'");
      }
      set.version = require_string(node, "version", context);
      set.source = optional_string(node, "source");
      set.sha256 = optional_string(node, "sha256");
      set.license = optional_string(node, "license");
      set.extract = string_list(node["extract"], context + ".extract");
      set.generate = optional_string(node, "generate");
      set.repo = optional_string(node, "repo");
      set.ref = optional_string(node, "ref");
      if (set.source.empty() && set.generate.empty()) {
        throw ManifestError(context + " needs a 'source' or a 'generate' entry");
      }
      if (!set.source.empty() && is_remote(set.source) && set.sha256.empty()) {
        throw ManifestError(context + " has a remote source and no 'sha256'");
      }
      manifest.sets.push_back(std::move(set));
    }
    return manifest;
  } catch (const ManifestError& e) {
    return fail(error_codes::kManifestInvalid, e.what());
  } catch (const std::exception& e) {
    return fail(error_codes::kManifestInvalid, std::string(e.what()));
  }
}

Result<Manifest> load_manifest_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kManifestInvalid,
                "cannot open manifest: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return load_manifest_string(buffer.str());
}

}  // namespace cf::refmirror
