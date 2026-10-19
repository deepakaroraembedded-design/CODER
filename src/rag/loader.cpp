#include "cf/rag/loader.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace cf::rag {
namespace {

struct LoaderError : std::runtime_error {
  explicit LoaderError(const std::string& message) : std::runtime_error(message) {}
};

std::string require_string(const YAML::Node& map, const char* key,
                           const std::string& context) {
  const YAML::Node node = map[key];
  if (!node || node.IsNull()) {
    throw LoaderError(context + " is missing required field '" + key + "'");
  }
  if (!node.IsScalar()) {
    throw LoaderError(context + "." + key + " must be a scalar");
  }
  std::string value = node.as<std::string>();
  if (value.empty()) {
    throw LoaderError(context + "." + key + " must not be empty");
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

// Epoch seconds or YYYY-MM-DD.
UnixTime parse_time(const YAML::Node& node, const std::string& context) {
  if (!node || node.IsNull()) {
    return 0;
  }
  if (!node.IsScalar()) {
    throw LoaderError(context + " must be a date or an epoch number");
  }
  const std::string text = node.as<std::string>();
  if (text.empty()) {
    return 0;
  }
  if (std::all_of(text.begin(), text.end(),
                  [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
    return std::stoll(text);
  }
  if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
    std::tm tm{};
    tm.tm_year = std::stoi(text.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(text.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(text.substr(8, 2));
    return static_cast<UnixTime>(::timegm(&tm));
  }
  throw LoaderError(context + " must be an epoch number or YYYY-MM-DD, got '" + text + "'");
}

// Returns the front matter and the body.
std::pair<std::string, std::string> split_front_matter(std::string_view text,
                                                       const std::string& context) {
  std::size_t pos = 0;
  const std::size_t first_end = text.find('\n');
  std::string_view first_line =
      first_end == std::string_view::npos ? text : text.substr(0, first_end);
  if (!first_line.empty() && first_line.back() == '\r') {
    first_line.remove_suffix(1);
  }
  if (first_line != "---") {
    throw LoaderError(context + ": document has no YAML front matter");
  }
  pos = first_end + 1;
  while (pos < text.size()) {
    const std::size_t line_end = text.find('\n', pos);
    std::string_view line = line_end == std::string_view::npos
                                ? text.substr(pos)
                                : text.substr(pos, line_end - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line == "---" || line == "...") {
      const std::string front(text.substr(0, pos));
      const std::string body(text.substr(line_end == std::string_view::npos
                                             ? text.size()
                                             : line_end + 1));
      return {front, body};
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    pos = line_end + 1;
  }
  throw LoaderError(context + ": front matter is not terminated by '---'");
}

}  // namespace

Result<Document> load_document_string(std::string_view text,
                                      const std::string& source_path) {
  try {
    auto [front_matter, body] = split_front_matter(text, source_path);
    YAML::Node root;
    try {
      root = YAML::Load(front_matter);
    } catch (const YAML::Exception& e) {
      throw LoaderError(source_path + ": cannot parse front matter: " + e.what());
    }
    if (!root || !root.IsMap()) {
      throw LoaderError(source_path + ": front matter must be a YAML mapping");
    }

    Document document;
    document.doc_id = require_string(root, "doc_id", "front matter");
    document.version = require_string(root, "version", "front matter");
    const std::string type_text = require_string(root, "type", "front matter");
    const auto type = doc_type_from_string(type_text);
    if (!type) {
      throw LoaderError(source_path + ": unknown document type '" + type_text + "'");
    }
    document.type = *type;
    document.owner = optional_string(root, "owner");
    const std::string classification = optional_string(root, "classification");
    if (!classification.empty()) {
      document.classification = classification;
    }
    const std::string scope = optional_string(root, "scope");
    if (!scope.empty()) {
      document.scope = scope;
    }
    document.scope_value = optional_string(root, "scope_value");
    document.effective_from = parse_time(root["effective_from"], "effective_from");
    document.effective_to = parse_time(root["effective_to"], "effective_to");
    const std::string status = optional_string(root, "status");
    if (!status.empty()) {
      document.status = status;
    }
    document.license = optional_string(root, "license");
    document.topic = optional_string(root, "topic");
    document.source = optional_string(root, "source");
    if (document.source.empty()) {
      document.source = source_path;
    }
    document.text = std::move(body);
    if (document.text.empty()) {
      throw LoaderError(source_path + ": document body is empty");
    }
    return document;
  } catch (const LoaderError& e) {
    return fail(error_codes::kInvalidDocument, e.what());
  } catch (const std::exception& e) {
    return fail(error_codes::kInvalidDocument,
                source_path + ": " + std::string(e.what()));
  }
}

Result<std::vector<Document>> load_document_dir(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return fail(error_codes::kInvalidDocument,
                "documents directory not found: " + dir.string());
  }
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string extension = entry.path().extension().string();
    if (extension == ".md" || extension == ".markdown") {
      files.push_back(entry.path());
    }
  }
  if (ec) {
    return fail(error_codes::kInvalidDocument,
                "cannot list documents directory: " + dir.string());
  }
  std::sort(files.begin(), files.end());
  std::vector<Document> documents;
  for (const auto& file : files) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return fail(error_codes::kInvalidDocument,
                  "cannot open document: " + file.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto document = load_document_string(buffer.str(), file.string());
    if (!document) {
      return tl::make_unexpected(document.error());
    }
    documents.push_back(std::move(*document));
  }
  return documents;
}

}  // namespace cf::rag
