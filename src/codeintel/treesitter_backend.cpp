#include "cf/codeintel/treesitter_backend.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include <tree_sitter/api.h>

#include "cf/common/hash.hpp"

// The vendored C grammar (third_party/tree-sitter-c, see VERSIONS).
extern "C" const TSLanguage* tree_sitter_c(void);

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

std::string node_text(std::string_view source, TSNode node) {
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_end_byte(node);
  return std::string(source.substr(begin, end - begin));
}

std::string rtrim(std::string text) {
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
          text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

std::uint32_t node_line(TSNode node) { return ts_node_start_point(node).row + 1; }

// Strips the comment markers (/** ... */, /* ... */, // ...) from a doc
// comment, keeping the text.
std::string clean_comment(std::string text) {
  if (text.rfind("/**", 0) == 0 || text.rfind("/*!", 0) == 0) {
    text.erase(0, 3);
  } else if (text.rfind("/*", 0) == 0) {
    text.erase(0, 2);
  } else if (text.rfind("//", 0) == 0) {
    text.erase(0, 2);
  }
  if (text.size() >= 2 && text.compare(text.size() - 2, 2, "*/") == 0) {
    text.erase(text.size() - 2);
  }
  std::string cleaned;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
      continue;
    }
    if (line[first] == '*') {
      ++first;  // leading '*' of a block-comment continuation line
      if (first < line.size() && line[first] == ' ') {
        ++first;
      }
    }
    if (!cleaned.empty()) {
      cleaned += '\n';
    }
    cleaned += rtrim(line.substr(first));
  }
  return cleaned;
}

// The doc comment immediately preceding a symbol node, when one exists.
std::string doc_comment(std::string_view source, TSNode node) {
  const TSNode previous = ts_node_prev_named_sibling(node);
  if (ts_node_is_null(previous) ||
      std::strcmp(ts_node_type(previous), "comment") != 0) {
    return {};
  }
  const std::uint32_t gap_begin = ts_node_end_byte(previous);
  const std::uint32_t gap_end = ts_node_start_byte(node);
  const std::string_view gap = source.substr(gap_begin, gap_end - gap_begin);
  const bool only_whitespace = std::all_of(gap.begin(), gap.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  });
  if (!only_whitespace) {
    return {};
  }
  return clean_comment(node_text(source, previous));
}

// Descends a declarator chain (pointer/array/function/parenthesized
// declarators) to the identifier that names the declared entity; returns a
// null node for abstract (unnamed) declarators.
TSNode declarator_name(TSNode declarator) {
  while (!ts_node_is_null(declarator)) {
    const std::string_view type = ts_node_type(declarator);
    if (type == "identifier" || type == "type_identifier" ||
        type == "field_identifier") {
      return declarator;
    }
    declarator = ts_node_child_by_field_name(declarator, "declarator",
                                             static_cast<std::uint32_t>(10));
  }
  return declarator;
}

// Per-file extraction state threaded through the recursive walk.
struct Extraction {
  std::string_view source;
  std::string path;  // root-relative
  SymbolGraph* graph;
  std::string current_caller;  // empty at file scope
};

void visit(TSNode node, Extraction& ex);

// A function_definition: the symbol, then the body walked for calls.
void extract_function_definition(TSNode node, Extraction& ex) {
  const TSNode declarator =
      ts_node_child_by_field_name(node, "declarator", 10);
  const TSNode name = declarator_name(declarator);
  if (!ts_node_is_null(name)) {
    Symbol symbol;
    symbol.name = node_text(ex.source, name);
    symbol.qualified = symbol.name;  // C has no namespaces
    symbol.kind = SymbolKind::Function;
    symbol.file = ex.path;
    symbol.line = node_line(node);
    symbol.is_definition = true;
    symbol.doc = doc_comment(ex.source, node);
    const TSNode body = ts_node_child_by_field_name(node, "body", 4);
    const std::uint32_t begin = ts_node_start_byte(node);
    const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                    : ts_node_start_byte(body);
    symbol.signature = rtrim(std::string(ex.source.substr(begin, end - begin)));
    ex.graph->symbols.push_back(std::move(symbol));
  }

  // Walk the body collecting call edges attributed to this function.
  const std::string previous_caller = std::move(ex.current_caller);
  ex.current_caller =
      ts_node_is_null(name) ? previous_caller : node_text(ex.source, name);
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(node, i), ex);
  }
  ex.current_caller = previous_caller;
}

// A file-scope declaration: function prototypes and global variables.
void extract_declaration(TSNode node, Extraction& ex) {
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode child = ts_node_named_child(node, i);
    const std::string_view type = ts_node_type(child);
    if (type == "function_declarator") {
      // A plain prototype declares the name directly; anything wrapped (for
      // example a function pointer) is a variable instead.
      const TSNode inner =
          ts_node_child_by_field_name(child, "declarator", 10);
      if (ts_node_is_null(inner) ||
          std::string_view(ts_node_type(inner)) != "identifier") {
        continue;
      }
      Symbol symbol;
      symbol.name = node_text(ex.source, inner);
      symbol.qualified = symbol.name;
      symbol.kind = SymbolKind::Function;
      symbol.file = ex.path;
      symbol.line = node_line(node);
      symbol.is_definition = false;
      symbol.doc = doc_comment(ex.source, node);
      symbol.signature = rtrim(node_text(ex.source, node));
      ex.graph->symbols.push_back(std::move(symbol));
    } else if (type == "identifier" || type == "init_declarator" ||
               type == "pointer_declarator" || type == "array_declarator") {
      const TSNode name = declarator_name(child);
      if (ts_node_is_null(name)) {
        continue;
      }
      Symbol symbol;
      symbol.name = node_text(ex.source, name);
      symbol.qualified = symbol.name;
      symbol.kind = SymbolKind::Variable;
      symbol.file = ex.path;
      symbol.line = node_line(node);
      symbol.is_definition = true;
      symbol.signature = rtrim(node_text(ex.source, node));
      ex.graph->symbols.push_back(std::move(symbol));
    }
  }
}

void extract_type_block(TSNode node, Extraction& ex, SymbolKind kind,
                        std::string_view keyword) {
  const TSNode name = ts_node_child_by_field_name(node, "name", 4);
  const TSNode body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(name) || ts_node_is_null(body)) {
    return;  // a reference ("struct Foo *"), not a definition
  }
  Symbol symbol;
  symbol.name = node_text(ex.source, name);
  symbol.qualified = symbol.name;
  symbol.kind = kind;
  symbol.file = ex.path;
  symbol.line = node_line(node);
  symbol.is_definition = true;
  symbol.doc = doc_comment(ex.source, node);
  symbol.signature = std::string(keyword) + " " + symbol.name;
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_typedef(TSNode node, Extraction& ex) {
  const TSNode name =
      declarator_name(ts_node_child_by_field_name(node, "declarator", 10));
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = node_text(ex.source, name);
  symbol.qualified = symbol.name;
  symbol.kind = SymbolKind::Typedef;
  symbol.file = ex.path;
  symbol.line = node_line(node);
  symbol.is_definition = true;
  symbol.doc = doc_comment(ex.source, node);
  symbol.signature = rtrim(node_text(ex.source, node));
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_macro(TSNode node, Extraction& ex) {
  const TSNode name = ts_node_child_by_field_name(node, "name", 4);
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = node_text(ex.source, name);
  symbol.qualified = symbol.name;
  symbol.kind = SymbolKind::Macro;
  symbol.file = ex.path;
  symbol.line = node_line(node);
  symbol.is_definition = true;
  symbol.signature = rtrim(node_text(ex.source, node));
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_include(TSNode node, Extraction& ex) {
  const TSNode path = ts_node_child_by_field_name(node, "path", 4);
  if (ts_node_is_null(path)) {
    return;
  }
  const std::string_view type = ts_node_type(path);
  std::string text = node_text(ex.source, path);
  IncludeEdge include;
  include.file = ex.path;
  include.line = node_line(node);
  if (type == "system_lib_string") {
    include.system = true;
  } else if (type != "string_literal") {
    return;  // computed include (#include MACRO): not a static path
  }
  // Strip the quotes or angle brackets.
  if (text.size() >= 2) {
    text = text.substr(1, text.size() - 2);
  }
  include.included = std::move(text);
  ex.graph->includes.push_back(std::move(include));
}

void extract_call(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;  // calls in file-scope initializers carry no caller
  }
  const TSNode function = ts_node_child_by_field_name(node, "function", 8);
  if (ts_node_is_null(function)) {
    return;
  }
  const std::string_view type = ts_node_type(function);
  TSNode callee = function;
  if (type == "field_expression") {
    callee = ts_node_child_by_field_name(function, "field", 5);
  } else if (type != "identifier") {
    return;  // calls through arbitrary expressions have no static name
  }
  if (ts_node_is_null(callee)) {
    return;
  }
  CallEdge edge;
  edge.caller = ex.current_caller;
  edge.callee = node_text(ex.source, callee);
  edge.file = ex.path;
  edge.line = node_line(node);
  ex.graph->calls.push_back(std::move(edge));
}

void visit(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "function_definition") {
    extract_function_definition(node, ex);
    return;  // the body was walked with the caller set
  }
  if (!ex.current_caller.empty()) {
    if (type == "call_expression") {
      extract_call(node, ex);
    }
  } else if (type == "declaration") {
    extract_declaration(node, ex);
  } else if (type == "struct_specifier") {
    extract_type_block(node, ex, SymbolKind::Struct, "struct");
  } else if (type == "enum_specifier") {
    extract_type_block(node, ex, SymbolKind::Enum, "enum");
  } else if (type == "type_definition") {
    extract_typedef(node, ex);
  } else if (type == "preproc_def" || type == "preproc_function_def") {
    extract_macro(node, ex);
  } else if (type == "preproc_include") {
    extract_include(node, ex);
  }
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(node, i), ex);
  }
}

}  // namespace

struct TreeSitterBackend::Impl {
  Impl(const TSLanguage* ts_language, std::string language_id,
       std::vector<std::string> extensions)
      : ts_language(ts_language),
        language_id(std::move(language_id)),
        extensions(std::move(extensions)) {}

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
    Extraction extraction{source, relative, &graph, {}};
    visit(ts_tree_root_node(tree.get()), extraction);
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
};

TreeSitterBackend::TreeSitterBackend(const TSLanguage* ts_language,
                                     std::string language_id,
                                     std::vector<std::string> extensions)
    : impl_(std::make_unique<Impl>(ts_language, std::move(language_id),
                                   std::move(extensions))) {}

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
    files.emplace_back(it->path(),
                       std::filesystem::relative(it->path(), root, ec).string());
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

std::unique_ptr<LanguageBackend> make_c_backend() {
  return std::make_unique<TreeSitterBackend>(tree_sitter_c(), "c",
                                             std::vector<std::string>{".c", ".h"});
}

}  // namespace cf::codeintel
