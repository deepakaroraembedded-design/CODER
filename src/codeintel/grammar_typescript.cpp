// cf-codeintel: TypeScript/JavaScript grammar extractor (tree-sitter, design
// section 6.5).
//
// Records classes, interfaces, enums, type aliases, functions and methods
// (qualified "Class.method"), arrow functions bound to a variable, imports
// and call edges with caller attribution.
#include <string>
#include <string_view>

#include "cf/codeintel/treesitter_backend.hpp"

#include "ts_util.hpp"

// The vendored TSX grammar parses TypeScript and JavaScript with JSX
// (third_party/tree-sitter-typescript, see VERSIONS).
extern "C" const TSLanguage* tree_sitter_tsx(void);

namespace cf::codeintel {
namespace {

using detail::Extraction;

void visit(TSNode node, Extraction& ex);

std::string signature_of(std::string_view source, TSNode node) {
  const TSNode body = detail::field(node, "body");
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                  : ts_node_start_byte(body);
  return detail::rtrim(std::string(source.substr(begin, end - begin)));
}

void add_symbol(TSNode node, Extraction& ex, std::string name,
                SymbolKind kind, std::string qualified) {
  Symbol symbol;
  symbol.name = std::move(name);
  symbol.qualified = std::move(qualified);
  symbol.kind = kind;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.signature = signature_of(ex.source, node);
  symbol.doc = detail::doc_comment(ex.source, node);
  symbol.is_definition = true;
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_callable(TSNode node, Extraction& ex, std::string name,
                      std::string qualified, SymbolKind kind) {
  add_symbol(node, ex, std::move(name), kind, std::move(qualified));
  const std::string caller = ex.graph->symbols.back().qualified;
  const TSNode body = detail::field(node, "body");
  if (ts_node_is_null(body)) {
    return;
  }
  const std::string previous = std::move(ex.current_caller);
  ex.current_caller = caller;
  const std::uint32_t count = ts_node_named_child_count(body);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(body, i), ex);
  }
  ex.current_caller = previous;
}

void extract_class_like(TSNode node, Extraction& ex, SymbolKind kind) {
  const TSNode name_node = detail::field(node, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  const std::string name = detail::node_text(ex.source, name_node);
  const std::string qualified =
      ex.current_scope.empty() ? name : ex.current_scope + "." + name;
  add_symbol(node, ex, name, kind, qualified);
  const TSNode body = detail::field(node, "body");
  if (ts_node_is_null(body)) {
    return;
  }
  const std::string previous_scope = std::move(ex.current_scope);
  ex.current_scope = qualified;
  const std::uint32_t count = ts_node_named_child_count(body);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(body, i), ex);
  }
  ex.current_scope = previous_scope;
}

void extract_method(TSNode node, Extraction& ex) {
  const TSNode name_node = detail::field(node, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  std::string name = detail::node_text(ex.source, name_node);
  const std::string qualified =
      ex.current_scope.empty() ? name : ex.current_scope + "." + name;
  extract_callable(node, ex, std::move(name), qualified, SymbolKind::Method);
}

void extract_function(TSNode node, Extraction& ex) {
  const TSNode name_node = detail::field(node, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  std::string name = detail::node_text(ex.source, name_node);
  const std::string qualified =
      ex.current_scope.empty() ? name : ex.current_scope + "." + name;
  extract_callable(node, ex, std::move(name), qualified, SymbolKind::Function);
}

void extract_type_alias(TSNode node, Extraction& ex) {
  const TSNode name_node = detail::field(node, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  std::string name = detail::node_text(ex.source, name_node);
  add_symbol(node, ex, name, SymbolKind::Typedef, name);
}

void extract_variable(TSNode node, Extraction& ex) {
  if (!ex.current_caller.empty() || !ex.current_scope.empty()) {
    return;  // only top-level const/let function bindings
  }
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode declarator = ts_node_named_child(node, i);
    if (std::string_view(ts_node_type(declarator)) != "variable_declarator") {
      continue;
    }
    const TSNode name_node = detail::field(declarator, "name");
    const TSNode value = detail::field(declarator, "value");
    if (ts_node_is_null(name_node) || ts_node_is_null(value)) {
      continue;
    }
    const std::string_view value_type = ts_node_type(value);
    if (value_type != "arrow_function" && value_type != "function_expression" &&
        value_type != "function") {
      continue;
    }
    const std::string name = detail::node_text(ex.source, name_node);
    add_symbol(declarator, ex, name, SymbolKind::Function, name);
    const std::string caller = ex.graph->symbols.back().qualified;
    const TSNode body = detail::field(value, "body");
    if (ts_node_is_null(body)) {
      continue;
    }
    const std::string previous = std::move(ex.current_caller);
    ex.current_caller = caller;
    const std::uint32_t children = ts_node_named_child_count(body);
    for (std::uint32_t child = 0; child < children; ++child) {
      visit(ts_node_named_child(body, child), ex);
    }
    ex.current_caller = previous;
  }
}

void extract_import(TSNode node, Extraction& ex) {
  const TSNode source = detail::field(node, "source");
  if (ts_node_is_null(source)) {
    return;
  }
  std::string text = detail::node_text(ex.source, source);
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'')) {
    text = text.substr(1, text.size() - 2);
  }
  ex.graph->includes.push_back(
      IncludeEdge{ex.path, std::move(text), false, detail::node_line(node)});
}

void extract_call(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  const TSNode function = detail::field(node, "function");
  if (ts_node_is_null(function)) {
    return;
  }
  const std::string_view type = ts_node_type(function);
  std::string callee;
  if (type == "identifier" || type == "property_identifier") {
    callee = detail::node_text(ex.source, function);
  } else if (type == "member_expression") {
    const TSNode property = detail::field(function, "property");
    if (!ts_node_is_null(property)) {
      callee = detail::node_text(ex.source, property);
    }
  }
  if (callee.empty()) {
    return;
  }
  ex.graph->calls.push_back(
      CallEdge{ex.current_caller, std::move(callee), ex.path,
               detail::node_line(node)});
}

void extract_new(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  TSNode constructor = detail::field(node, "constructor");
  if (ts_node_is_null(constructor)) {
    return;
  }
  while (!ts_node_is_null(constructor) &&
         std::string_view(ts_node_type(constructor)) == "member_expression") {
    constructor = detail::field(constructor, "property");
  }
  if (ts_node_is_null(constructor)) {
    return;
  }
  ex.graph->calls.push_back(
      CallEdge{ex.current_caller, detail::node_text(ex.source, constructor),
               ex.path, detail::node_line(node)});
}

void visit(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "class_declaration" || type == "class" ||
      type == "abstract_class_declaration") {
    extract_class_like(node, ex, SymbolKind::Class);
    return;
  }
  if (type == "interface_declaration") {
    extract_class_like(node, ex, SymbolKind::Interface);
    return;
  }
  if (type == "enum_declaration") {
    extract_class_like(node, ex, SymbolKind::Enum);
    return;
  }
  if (type == "type_alias_declaration") {
    extract_type_alias(node, ex);
    return;
  }
  if (type == "function_declaration" ||
      type == "generator_function_declaration") {
    extract_function(node, ex);
    return;
  }
  if (type == "method_definition") {
    extract_method(node, ex);
    return;
  }
  if (type == "lexical_declaration" || type == "variable_declaration") {
    extract_variable(node, ex);
  } else if (type == "import_statement") {
    extract_import(node, ex);
  } else if (type == "call_expression") {
    extract_call(node, ex);
  } else if (type == "new_expression") {
    extract_new(node, ex);
  }

  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(node, i), ex);
  }
}

void extract_typescript(TSNode root, Extraction& ex) { visit(root, ex); }

}  // namespace

std::unique_ptr<LanguageBackend> make_typescript_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_tsx(), "typescript",
      std::vector<std::string>{".ts", ".tsx", ".mts", ".cts"}, extract_typescript);
}

std::unique_ptr<LanguageBackend> make_javascript_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_tsx(), "javascript",
      std::vector<std::string>{".js", ".jsx", ".mjs", ".cjs"}, extract_typescript);
}

}  // namespace cf::codeintel
