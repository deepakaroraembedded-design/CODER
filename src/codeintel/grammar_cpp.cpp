// cf-codeintel: C++ grammar extractor (tree-sitter-cpp, design section 6.5).
//
// This is the tree-sitter fallback used when the Clang LibTooling back end is
// not available in the build environment. It records namespaces, classes,
// structs, functions and methods (qualified "Class::method"), fields,
// includes and call edges with caller attribution.
#include <string>
#include <string_view>

#include "cf/codeintel/treesitter_backend.hpp"

#include "ts_util.hpp"

// The vendored C++ grammar (third_party/tree-sitter-cpp, see VERSIONS).
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace cf::codeintel {
namespace {

using detail::Extraction;

void visit(TSNode node, Extraction& ex);

std::string joined_scope(const std::string& scope, const std::string& name) {
  return scope.empty() ? name : scope + "::" + name;
}

TSNode descend_declarator(TSNode node) {
  for (int depth = 0; depth < 16; ++depth) {
    const std::string_view type = ts_node_type(node);
    if (type == "function_declarator") {
      return node;
    }
    if (type == "pointer_declarator" || type == "reference_declarator" ||
        type == "array_declarator" || type == "parenthesized_declarator" ||
        type == "attributed_declarator") {
      node = detail::field(node, "declarator");
      continue;
    }
    return TSNode{};
  }
  return TSNode{};
}

std::string declarator_name(std::string_view source, TSNode node) {
  if (ts_node_is_null(node)) {
    return {};
  }
  const std::string_view type = ts_node_type(node);
  if (type == "identifier" || type == "field_identifier" ||
      type == "destructor_name" || type == "operator_name") {
    return detail::node_text(source, node);
  }
  if (type == "qualified_identifier") {
    const TSNode name = detail::field(node, "name");
    return ts_node_is_null(name) ? detail::node_text(source, node)
                                 : detail::node_text(source, name);
  }
  if (type == "template_function" || type == "template_method" ||
      type == "function_declarator" || type == "pointer_declarator" ||
      type == "reference_declarator" || type == "parenthesized_declarator") {
    return declarator_name(source, detail::field(node, "declarator"));
  }
  for (std::uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
    const std::string name =
        declarator_name(source, ts_node_named_child(node, i));
    if (!name.empty()) {
      return name;
    }
  }
  return {};
}

void add_symbol(TSNode node, Extraction& ex, std::string name,
                SymbolKind kind, std::string qualified) {
  Symbol symbol;
  symbol.name = std::move(name);
  symbol.qualified = std::move(qualified);
  symbol.kind = kind;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  const TSNode body = detail::field(node, "body");
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                  : ts_node_start_byte(body);
  symbol.signature = detail::rtrim(std::string(ex.source.substr(begin, end - begin)));
  symbol.doc = detail::doc_comment(ex.source, node);
  symbol.is_definition = !ts_node_is_null(body);
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_function(TSNode node, Extraction& ex) {
  const TSNode declarator = descend_declarator(detail::field(node, "declarator"));
  if (ts_node_is_null(declarator)) {
    return;
  }
  const std::string name =
      declarator_name(ex.source, detail::field(declarator, "declarator"));
  if (name.empty()) {
    return;
  }
  const std::string qualified = joined_scope(ex.current_scope, name);
  add_symbol(node, ex, name,
             ex.current_class.empty() ? SymbolKind::Function : SymbolKind::Method,
             qualified);
  const TSNode body = detail::field(node, "body");
  if (ts_node_is_null(body)) {
    return;
  }
  const std::string previous = std::move(ex.current_caller);
  ex.current_caller = qualified;
  const std::uint32_t count = ts_node_named_child_count(body);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(body, i), ex);
  }
  ex.current_caller = previous;
}

void extract_class(TSNode node, Extraction& ex, SymbolKind kind) {
  const TSNode name_node = detail::field(node, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  const std::string name = detail::node_text(ex.source, name_node);
  const std::string qualified = joined_scope(ex.current_scope, name);
  add_symbol(node, ex, name, kind, qualified);
  const TSNode body = detail::field(node, "body");
  if (ts_node_is_null(body)) {
    return;
  }
  const std::string previous_scope = std::move(ex.current_scope);
  const std::string previous_class = std::move(ex.current_class);
  ex.current_scope = qualified;
  ex.current_class = qualified;
  const std::uint32_t count = ts_node_named_child_count(body);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(body, i), ex);
  }
  ex.current_scope = previous_scope;
  ex.current_class = previous_class;
}

void extract_namespace(TSNode node, Extraction& ex) {
  const TSNode name_node = detail::field(node, "name");
  const std::string name = ts_node_is_null(name_node)
                               ? std::string()
                               : detail::node_text(ex.source, name_node);
  const std::string qualified = joined_scope(ex.current_scope, name);
  if (!name.empty()) {
    add_symbol(node, ex, name, SymbolKind::Namespace, qualified);
  }
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

void extract_field(TSNode node, Extraction& ex) {
  const TSNode declarator = detail::field(node, "declarator");
  if (ts_node_is_null(declarator)) {
    return;
  }
  const TSNode function = descend_declarator(declarator);
  if (!ts_node_is_null(function)) {
    const std::string name =
        declarator_name(ex.source, detail::field(function, "declarator"));
    if (!name.empty()) {
      add_symbol(node, ex, name, SymbolKind::Method,
                 joined_scope(ex.current_scope, name));
    }
    return;
  }
  const std::string name = declarator_name(ex.source, declarator);
  if (!name.empty()) {
    add_symbol(node, ex, name, SymbolKind::Variable,
               joined_scope(ex.current_scope, name));
  }
}

void extract_declaration(TSNode node, Extraction& ex) {
  const TSNode declarator = detail::field(node, "declarator");
  if (ts_node_is_null(declarator)) {
    return;
  }
  const TSNode function = descend_declarator(declarator);
  if (!ts_node_is_null(function)) {
    const std::string name =
        declarator_name(ex.source, detail::field(function, "declarator"));
    if (!name.empty()) {
      add_symbol(node, ex, name,
                 ex.current_class.empty() ? SymbolKind::Function
                                          : SymbolKind::Method,
                 joined_scope(ex.current_scope, name));
    }
    return;
  }
  if (ex.current_scope.empty()) {
    const std::string name = declarator_name(ex.source, declarator);
    if (!name.empty()) {
      add_symbol(node, ex, name, SymbolKind::Variable, name);
    }
  }
}

void extract_include(TSNode node, Extraction& ex) {
  const TSNode path = detail::field(node, "path");
  if (ts_node_is_null(path)) {
    return;
  }
  const std::string_view type = ts_node_type(path);
  std::string text = detail::node_text(ex.source, path);
  const bool system = type == "system_lib_string";
  if (text.size() >= 2) {
    text = text.substr(1, text.size() - 2);
  }
  ex.graph->includes.push_back(
      IncludeEdge{ex.path, std::move(text), system, detail::node_line(node)});
}

void extract_call(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  const TSNode function = detail::field(node, "function");
  if (ts_node_is_null(function)) {
    return;
  }
  std::string callee;
  const std::string_view type = ts_node_type(function);
  if (type == "identifier" || type == "field_identifier") {
    callee = detail::node_text(ex.source, function);
  } else if (type == "field_expression") {
    const TSNode field = detail::field(function, "field");
    if (!ts_node_is_null(field)) {
      callee = detail::node_text(ex.source, field);
    }
  } else if (type == "qualified_identifier") {
    callee = declarator_name(ex.source, function);
  } else if (type == "template_function" || type == "template_method") {
    callee = declarator_name(ex.source, function);
  }
  if (callee.empty()) {
    return;
  }
  ex.graph->calls.push_back(
      CallEdge{ex.current_caller, std::move(callee), ex.path,
               detail::node_line(node)});
}

void visit(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "function_definition") {
    extract_function(node, ex);
    return;
  }
  if (type == "class_specifier") {
    extract_class(node, ex, SymbolKind::Class);
    return;
  }
  if (type == "struct_specifier") {
    extract_class(node, ex, SymbolKind::Struct);
    return;
  }
  if (type == "namespace_definition") {
    extract_namespace(node, ex);
    return;
  }
  if (type == "field_declaration") {
    extract_field(node, ex);
  } else if (type == "declaration") {
    extract_declaration(node, ex);
  } else if (type == "preproc_include") {
    extract_include(node, ex);
  } else if (type == "call_expression") {
    extract_call(node, ex);
  }
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(node, i), ex);
  }
}

void extract_cpp(TSNode root, Extraction& ex) { visit(root, ex); }

}  // namespace

std::unique_ptr<LanguageBackend> make_cpp_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_cpp(), "cpp",
      std::vector<std::string>{".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx"},
      extract_cpp);
}

}  // namespace cf::codeintel
