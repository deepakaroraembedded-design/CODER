#include "cf/codeintel/treesitter_backend.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "cf/common/hash.hpp"

namespace cf::codeintel {
namespace {

// RAII for the tree-sitter C API.
struct ParserDeleter {
  void operator()(TSParser* parser) const { ts_parser_delete(parser); }
};
struct TreeDeleter {
  void operator()(TSTree* tree) const { ts_tree_delete(tree); }
};
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

}  // namespace

struct TreeSitterBackend::Impl {
  Impl(const TSLanguage* ts_language, std::string language_id,
       std::vector<std::string> extensions, detail::ExtractFn extract)
      : ts_language(ts_language),
        language_id(std::move(language_id)),
        extensions(std::move(extensions)),
        extract(extract) {}

  // Parses one source file into the graph. The path is stored
  // root-relative.
  Status parse_file(const std::filesystem::path& file, const std::string& relative,
                    SymbolGraph& graph) const {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return fail(error_codes::kParseFailed, "cannot read " + file.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    const ParserPtr parser(ts_parser_new());
    if (parser == nullptr ||
        !ts_parser_set_language(parser.get(), ts_language)) {
      return fail(error_codes::kParseFailed,
                  "tree-sitter rejected the " + language_id + " grammar (ABI mismatch)");
    }
    const TreePtr tree(ts_parser_parse_string(
        parser.get(), nullptr, source.data(),
        static_cast<std::uint32_t>(source.size())));
    if (tree == nullptr) {
      return fail(error_codes::kParseFailed, "tree-sitter failed on " + file.string());
    }

    graph.files.push_back(FileRecord{relative, sha256_hex(source)});
    detail::Extraction extraction{source, relative, &graph, {}, {}};
    extract(ts_tree_root_node(tree.get()), extraction);
    return {};
  }

  Result<SymbolGraph> parse_files(const std::string& root,
                                  std::vector<std::pair<std::filesystem::path, std::string>>
                                      files) const {
    SymbolGraph graph;
    graph.root = root;
    graph.language = language_id;
    for (const auto& [file, relative] : files) {
      auto parsed = parse_file(file, relative, graph);
      if (!parsed) {
        return tl::make_unexpected(parsed.error());
      }
    }
    return graph;
  }

  const TSLanguage* ts_language;
  std::string language_id;
  std::vector<std::string> extensions;
  detail::ExtractFn extract;
};

TreeSitterBackend::TreeSitterBackend(const TSLanguage* ts_language,
                                     std::string language_id,
                                     std::vector<std::string> extensions,
                                     detail::ExtractFn extract)
    : impl_(std::make_unique<Impl>(ts_language, std::move(language_id),
                                   std::move(extensions), extract)) {}

TreeSitterBackend::~TreeSitterBackend() = default;

std::string_view TreeSitterBackend::language() const { return impl_->language_id; }

Result<SymbolGraph> TreeSitterBackend::index(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return fail(error_codes::kParseFailed, root.string() + " is not a directory");
  }
  std::vector<std::pair<std::filesystem::path, std::string>> files;
  for (std::filesystem::recursive_directory_iterator it(
           root, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec || !it->is_regular_file(ec)) {
      continue;
    }
    const std::string extension = it->path().extension().string();
    const bool recognised = std::find(impl_->extensions.begin(),
                                      impl_->extensions.end(),
                                      extension) != impl_->extensions.end();
    if (!recognised) {
      continue;
    }
    auto relative = std::filesystem::relative(it->path(), root, ec);
    if (ec) {
      continue;
    }
    files.emplace_back(it->path(), relative.string());
  }
  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
  return impl_->parse_files(root.string(), std::move(files));
}

Result<SymbolGraph> TreeSitterBackend::update(
    const SymbolGraph& graph, const std::vector<std::filesystem::path>& changed) {
  std::vector<std::pair<std::filesystem::path, std::string>> files;
  for (const auto& path : changed) {
    const std::string extension = path.extension().string();
    const bool recognised = std::find(impl_->extensions.begin(),
                                      impl_->extensions.end(),
                                      extension) != impl_->extensions.end();
    if (recognised) {
      files.emplace_back(std::filesystem::path(graph.root) / path, path.string());
    }
  }
  return impl_->parse_files(graph.root, std::move(files));
}

Result<std::vector<Diagnostic>> TreeSitterBackend::check_architecture(
    const SymbolGraph& graph, const ArchitectureRules& rules) {
  return check_layer_rules(graph, rules);
}

}  // namespace cf::codeintel
