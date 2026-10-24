// cf-codeintel: the C grammar extractor (tree-sitter-c, design section 6.5).
#include <string_view>

#include "cf/codeintel/treesitter_backend.hpp"

#include "ts_util.hpp"

// The vendored C grammar (third_party/tree-sitter-c, see VERSIONS).
extern "C" const TSLanguage* tree_sitter_c(void);

namespace cf::codeintel {
namespace {

using detail::Extraction;
using detail::doc_comment;

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
    declarator = detail::field(declarator, "declarator");
  }
  return declarator;
}

void visit(TSNode node, Extraction& ex);

// A function_definition: the symbol, then the body walked for calls.
void extract_function_definition(TSNode node, Extraction& ex) {
  const TSNode declarator = detail::field(node, "declarator");
  const TSNode name = declarator_name(declarator);
  if (!ts_node_is_null(name)) {
    Symbol symbol;
    symbol.name = detail::node_text(ex.source, name);
    symbol.qualified = symbol.name;  // C has no namespaces
    symbol.kind = SymbolKind::Function;
    symbol.file = ex.path;
    symbol.line = detail::node_line(node);
    symbol.is_definition = true;
    symbol.doc = doc_comment(ex.source, node);
    const TSNode body = detail::field(node, "body");
    const std::uint32_t begin = ts_node_start_byte(node);
    const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                    : ts_node_start_byte(body);
    symbol.signature = detail::rtrim(std::string(ex.source.substr(begin, end - begin)));
    ex.graph->symbols.push_back(std::move(symbol));
  }

  // Walk the body collecting call edges attributed to this function.
  const std::string previous_caller = std::move(ex.current_caller);
  ex.current_caller =
      ts_node_is_null(name) ? previous_caller : detail::node_text(ex.source, name);
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
      const TSNode inner = detail::field(child, "declarator");
      if (ts_node_is_null(inner) ||
          std::string_view(ts_node_type(inner)) != "identifier") {
        continue;
      }
      Symbol symbol;
      symbol.name = detail::node_text(ex.source, inner);
      symbol.qualified = symbol.name;
      symbol.kind = SymbolKind::Function;
      symbol.file = ex.path;
      symbol.line = detail::node_line(node);
      symbol.is_definition = false;
      symbol.doc = doc_comment(ex.source, node);
      symbol.signature = detail::rtrim(detail::node_text(ex.source, node));
      ex.graph->symbols.push_back(std::move(symbol));
    } else if (type == "identifier" || type == "init_declarator" ||
               type == "pointer_declarator" || type == "array_declarator") {
      const TSNode name = declarator_name(child);
      if (ts_node_is_null(name)) {
        continue;
      }
      Symbol symbol;
      symbol.name = detail::node_text(ex.source, name);
      symbol.qualified = symbol.name;
      symbol.kind = SymbolKind::Variable;
      symbol.file = ex.path;
      symbol.line = detail::node_line(node);
      symbol.is_definition = true;
      symbol.signature = detail::rtrim(detail::node_text(ex.source, node));
      ex.graph->symbols.push_back(std::move(symbol));
    }
  }
}

void extract_type_block(TSNode node, Extraction& ex, SymbolKind kind,
                        std::string_view keyword) {
  const TSNode name = detail::field(node, "name");
  const TSNode body = detail::field(node, "body");
  if (ts_node_is_null(name) || ts_node_is_null(body)) {
    return;  // a reference ("struct Foo *"), not a definition
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.qualified = symbol.name;
  symbol.kind = kind;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  symbol.doc = doc_comment(ex.source, node);
  symbol.signature = std::string(keyword) + " " + symbol.name;
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_typedef(TSNode node, Extraction& ex) {
  const TSNode name = declarator_name(detail::field(node, "declarator"));
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.qualified = symbol.name;
  symbol.kind = SymbolKind::Typedef;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  symbol.doc = doc_comment(ex.source, node);
  symbol.signature = detail::rtrim(detail::node_text(ex.source, node));
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_macro(TSNode node, Extraction& ex) {
  const TSNode name = detail::field(node, "name");
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.qualified = symbol.name;
  symbol.kind = SymbolKind::Macro;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  symbol.signature = detail::rtrim(detail::node_text(ex.source, node));
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_include(TSNode node, Extraction& ex) {
  const TSNode path = detail::field(node, "path");
  if (ts_node_is_null(path)) {
    return;
  }
  const std::string_view type = ts_node_type(path);
  std::string text = detail::node_text(ex.source, path);
  IncludeEdge include;
  include.file = ex.path;
  include.line = detail::node_line(node);
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
  const TSNode function = detail::field(node, "function");
  if (ts_node_is_null(function)) {
    return;
  }
  const std::string_view type = ts_node_type(function);
  TSNode callee = function;
  if (type == "field_expression") {
    callee = detail::field(function, "field");
  } else if (type != "identifier") {
    return;  // calls through arbitrary expressions have no static name
  }
  if (ts_node_is_null(callee)) {
    return;
  }
  CallEdge edge;
  edge.caller = ex.current_caller;
  edge.callee = detail::node_text(ex.source, callee);
  edge.file = ex.path;
  edge.line = detail::node_line(node);
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

void extract_c(TSNode root, Extraction& ex) { visit(root, ex); }

}  // namespace

std::unique_ptr<LanguageBackend> make_c_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_c(), "c", std::vector<std::string>{".c", ".h"}, extract_c);
}

}  // namespace cf::codeintel
