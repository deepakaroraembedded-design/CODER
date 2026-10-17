#include "cf/rag/catalog.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace cf::rag {
namespace {

std::vector<std::string> string_list(const YAML::Node& node, const std::string& context) {
  std::vector<std::string> out;
  if (!node) {
    return out;
  }
  if (!node.IsSequence()) {
    throw std::runtime_error(context + " must be a sequence");
  }
  for (const auto& item : node) {
    if (!item.IsScalar()) {
      throw std::runtime_error(context + " entries must be scalars");
    }
    out.push_back(item.as<std::string>());
  }
  return out;
}

std::string required_scalar(const YAML::Node& node, const std::string& key,
                            const std::string& context) {
  if (!node[key] || !node[key].IsScalar()) {
    throw std::runtime_error(context + " is missing required field '" + key + "'");
  }
  const std::string value = node[key].as<std::string>();
  if (value.empty()) {
    throw std::runtime_error(context + " field '" + key + "' cannot be empty");
  }
  return value;
}

}  // namespace

std::string PatternEntry::to_markdown() const {
  std::ostringstream out;
  out << "# " << id << " — " << name << "\n\n";
  out << "Level: " << level << "\n\n";
  if (!use_when.empty()) {
    out << "Use when:\n";
    for (const auto& item : use_when) {
      out << "- " << item << "\n";
    }
    out << "\n";
  }
  if (!avoid_when.empty()) {
    out << "Avoid when:\n";
    for (const auto& item : avoid_when) {
      out << "- " << item << "\n";
    }
    out << "\n";
  }
  if (!company_notes.empty()) {
    out << "Company notes: " << company_notes << "\n\n";
  }
  if (!reference_impl.empty()) {
    out << "Reference implementation: " << reference_impl << "\n\n";
  }
  if (!conformance_checks.empty()) {
    out << "Conformance checks:\n";
    for (const auto& [language, check] : conformance_checks) {
      out << "- " << language << ": " << check << "\n";
    }
  }
  return out.str();
}

Result<PatternEntry> parse_pattern_entry(std::string_view yaml,
                                         const std::string& source_path) {
  try {
    YAML::Node root = YAML::Load(std::string(yaml));
    if (!root || !root.IsMap()) {
      throw std::runtime_error(source_path + ": the catalog entry must be a mapping");
    }
    PatternEntry entry;
    entry.id = required_scalar(root, "id", source_path);
    entry.name = required_scalar(root, "name", source_path);
    entry.level = required_scalar(root, "level", source_path);
    if (entry.level != "system" && entry.level != "component") {
      throw std::runtime_error(source_path +
                               ": level must be 'system' or 'component', got '" +
                               entry.level + "'");
    }
    entry.use_when = string_list(root["use_when"], source_path + ".use_when");
    entry.avoid_when = string_list(root["avoid_when"], source_path + ".avoid_when");
    if (root["company_notes"] && root["company_notes"].IsScalar()) {
      entry.company_notes = root["company_notes"].as<std::string>();
    }
    if (root["reference_impl"] && root["reference_impl"].IsScalar()) {
      entry.reference_impl = root["reference_impl"].as<std::string>();
    }
    if (const YAML::Node checks = root["conformance_checks"]) {
      if (!checks.IsMap()) {
        throw std::runtime_error(source_path +
                                 ".conformance_checks must be a mapping");
      }
      for (const auto& item : checks) {
        if (!item.second.IsScalar()) {
          throw std::runtime_error(source_path +
                                   ".conformance_checks values must be scalars");
        }
        entry.conformance_checks[item.first.as<std::string>()] =
            item.second.as<std::string>();
      }
    }
    entry.source_path = source_path;
    return entry;
  } catch (const YAML::Exception& error) {
    return fail(error_codes::kCatalogParseFailed,
                source_path + ": cannot parse catalog entry: " + error.what());
  } catch (const std::exception& error) {
    return fail(error_codes::kCatalogParseFailed, error.what());
  }
}

Result<std::vector<PatternEntry>> load_pattern_catalog(
    const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return fail(error_codes::kCatalogParseFailed,
                "pattern catalog directory not found: " + dir.string());
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
    return fail(error_codes::kCatalogParseFailed,
                "cannot list pattern catalog: " + dir.string());
  }
  std::sort(files.begin(), files.end());

  std::vector<PatternEntry> catalog;
  std::set<std::string> ids;
  for (const auto& file : files) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return fail(error_codes::kCatalogParseFailed,
                  "cannot open pattern catalog entry: " + file.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto entry = parse_pattern_entry(buffer.str(), file.string());
    if (!entry) {
      return tl::make_unexpected(entry.error());
    }
    if (!ids.insert(entry->id).second) {
      return fail(error_codes::kCatalogParseFailed,
                  "duplicate pattern id '" + entry->id + "' in " + file.string());
    }
    catalog.push_back(std::move(*entry));
  }
  return catalog;
}

Document pattern_document(const PatternEntry& entry) {
  Document document;
  document.doc_id = "pattern-" + entry.id;
  document.version = "1";
  document.type = DocType::Pattern;
  document.owner = "cf-orchestrator";
  document.classification = "internal";
  document.scope = "company";
  document.status = "active";
  document.source = entry.source_path.empty()
                        ? "catalog://patterns/" + entry.id
                        : entry.source_path;
  document.text = entry.to_markdown();
  return document;
}

}  // namespace cf::rag
