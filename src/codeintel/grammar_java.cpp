// cf-codeintel: Java grammar extractor (tree-sitter-java, design section
// 6.5).
//
// Records classes, interfaces, enums, records, methods and constructors
// (qualified "Class.method"), fields, imports and call edges with caller
// attribution.
#include <string>
#include <string_view>

#include "cf/codeintel/treesitter_backend.hpp"

#include "ts_util.hpp"

// The vendored Java grammar (third_party/tree-sitter-java, see VERSIONS).
extern "C" const TSLanguage* tree_sitter_java(void);

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

void extract_callable(TSNode node, Extraction& ex, SymbolKind kind) {
  const TSNode name_node = detail::field(node, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  std::string name = detail::node_text(ex.source, name_node);
  const std::string qualified =
      ex.current_scope.empty() ? name : ex.current_scope + "." + name;
  add_symbol(node, ex, name, kind, qualified);
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

void extract_field(TSNode node, Extraction& ex) {
  const TSNode declarator = detail::field(node, "declarator");
  if (ts_node_is_null(declarator)) {
    return;
  }
  const TSNode name_node = detail::field(declarator, "name");
  if (ts_node_is_null(name_node)) {
    return;
  }
  std::string name = detail::node_text(ex.source, name_node);
  const std::string qualified =
      ex.current_scope.empty() ? name : ex.current_scope + "." + name;
  add_symbol(declarator, ex, name, SymbolKind::Variable, qualified);
}

void extract_import(TSNode node, Extraction& ex) {
  std::string text = detail::node_text(ex.source, node);
  const std::size_t begin = text.find("import ");
  if (begin != std::string::npos) {
    text.erase(0, begin + 7);
  }
  if (text.rfind("static ", 0) == 0) {
    text.erase(0, 7);
  }
  const std::size_t end = text.find(';');
  if (end != std::string::npos) {
    text.erase(end);
  }
  text = detail::rtrim(std::move(text));
  if (!text.empty()) {
    ex.graph->includes.push_back(
        IncludeEdge{ex.path, std::move(text), false, detail::node_line(node)});
  }
}

void extract_call(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  const TSNode name = detail::field(node, "name");
  if (ts_node_is_null(name)) {
    return;
  }
  ex.graph->calls.push_back(CallEdge{ex.current_caller,
                                     detail::node_text(ex.source, name), ex.path,
                                     detail::node_line(node)});
}

void extract_new(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  TSNode type = detail::field(node, "type");
  if (ts_node_is_null(type)) {
    return;
  }
  while (!ts_node_is_null(type) &&
         (std::string_view(ts_node_type(type)) == "generic_type" ||
          std::string_view(ts_node_type(type)) == "scoped_type_identifier")) {
    type = detail::field(type, "type");
  }
  if (ts_node_is_null(type)) {
    return;
  }
  std::string callee = detail::node_text(ex.source, type);
  const std::size_t dot = callee.rfind('.');
  if (dot != std::string::npos) {
    callee.erase(0, dot + 1);
  }
  ex.graph->calls.push_back(
      CallEdge{ex.current_caller, std::move(callee), ex.path,
               detail::node_line(node)});
}

void visit(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "class_declaration") {
    extract_class_like(node, ex, SymbolKind::Class);
    return;
  }
  if (type == "interface_declaration" || type == "annotation_type_declaration") {
    extract_class_like(node, ex, SymbolKind::Interface);
    return;
  }
  if (type == "enum_declaration") {
    extract_class_like(node, ex, SymbolKind::Enum);
    return;
  }
  if (type == "record_declaration") {
    extract_class_like(node, ex, SymbolKind::Struct);
    return;
  }
  if (type == "method_declaration" ||
      type == "constructor_declaration") {
    extract_callable(node, ex, SymbolKind::Method);
    return;
  }
  if (type == "field_declaration") {
    extract_field(node, ex);
  } else if (type == "import_declaration") {
    extract_import(node, ex);
  } else if (type == "method_invocation") {
    extract_call(node, ex);
  } else if (type == "object_creation_expression") {
    extract_new(node, ex);
  }

  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit(ts_node_named_child(node, i), ex);
  }
}

void extract_java(TSNode root, Extraction& ex) { visit(root, ex); }

}  // namespace

std::unique_ptr<LanguageBackend> make_java_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_java(), "java", std::vector<std::string>{".java"}, extract_java);
}

}  // namespace cf::codeintel
