// cf-codeintel: the Python grammar extractor (tree-sitter-python, design
// section 6.5).
//
// Records functions and classes (methods qualified "Class.method"),
// module-level variables, imports as include edges and call edges with
// caller attribution. Doc comments are the docstring convention: the first
// string statement of a body.
#include <string>
#include <string_view>

#include "cf/codeintel/treesitter_backend.hpp"

#include "ts_util.hpp"

// The vendored Python grammar (third_party/tree-sitter-python, see
// VERSIONS).
extern "C" const TSLanguage* tree_sitter_python(void);

namespace cf::codeintel {
namespace {

using detail::Extraction;

// The docstring of a body block: its first statement when it is a bare
// string (the Python documentation convention).
std::string docstring(std::string_view source, TSNode body) {
  if (ts_node_is_null(body)) {
    return {};
  }
  const TSNode first = ts_node_named_child(body, 0);
  if (ts_node_is_null(first) ||
      std::string_view(ts_node_type(first)) != "expression_statement") {
    return {};
  }
  const TSNode string = ts_node_named_child(first, 0);
  if (ts_node_is_null(string) ||
      std::string_view(ts_node_type(string)) != "string") {
    return {};
  }
  // The grammar splits strings into string_start/string_content/string_end
  // (all named); the content node holds the raw doc text.
  const std::uint32_t parts = ts_node_named_child_count(string);
  for (std::uint32_t i = 0; i < parts; ++i) {
    const TSNode child = ts_node_named_child(string, i);
    if (std::string_view(ts_node_type(child)) == "string_content") {
      return detail::rtrim(detail::node_text(source, child));
    }
  }
  return {};
}

void visit_py(TSNode node, Extraction& ex);

void extract_py_function(TSNode node, Extraction& ex) {
  const TSNode name = detail::field(node, "name");
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.qualified = ex.current_scope.empty()
                         ? symbol.name
                         : ex.current_scope + "." + symbol.name;
  symbol.kind = ex.current_scope.empty() ? SymbolKind::Function : SymbolKind::Method;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  const TSNode body = detail::field(node, "body");
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                  : ts_node_start_byte(body);
  symbol.signature = detail::rtrim(std::string(ex.source.substr(begin, end - begin)));
  symbol.doc = docstring(ex.source, body);
  ex.graph->symbols.push_back(std::move(symbol));

  // Walk the body collecting calls; nested definitions are not methods of
  // the enclosing class, so the scope clears inside the function.
  const std::string previous_caller = std::move(ex.current_caller);
  const std::string previous_scope = std::move(ex.current_scope);
  ex.current_caller = detail::node_text(ex.source, name);
  ex.current_scope.clear();
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit_py(ts_node_named_child(node, i), ex);
  }
  ex.current_caller = previous_caller;
  ex.current_scope = previous_scope;
}

void extract_py_class(TSNode node, Extraction& ex) {
  const TSNode name = detail::field(node, "name");
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.qualified = ex.current_scope.empty()
                         ? symbol.name
                         : ex.current_scope + "." + symbol.name;
  symbol.kind = SymbolKind::Class;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  const TSNode body = detail::field(node, "body");
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                  : ts_node_start_byte(body);
  symbol.signature = detail::rtrim(std::string(ex.source.substr(begin, end - begin)));
  symbol.doc = docstring(ex.source, body);
  ex.graph->symbols.push_back(std::move(symbol));

  // The class body is walked with the scope set, so direct members become
  // qualified methods.
  const std::string previous_scope = std::move(ex.current_scope);
  ex.current_scope = detail::node_text(ex.source, name);
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit_py(ts_node_named_child(node, i), ex);
  }
  ex.current_scope = previous_scope;
}

// A module-level (or class-body) assignment to plain identifiers.
void extract_py_assignment(TSNode node, Extraction& ex) {
  const TSNode left = detail::field(node, "left");
  if (ts_node_is_null(left) ||
      std::string_view(ts_node_type(left)) != "identifier") {
    return;  // tuple unpacking and attribute targets are not indexed
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, left);
  symbol.qualified = ex.current_scope.empty()
                         ? symbol.name
                         : ex.current_scope + "." + symbol.name;
  symbol.kind = SymbolKind::Variable;
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  symbol.signature = detail::rtrim(detail::node_text(ex.source, node));
  ex.graph->symbols.push_back(std::move(symbol));
}

void extract_py_import(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "import_from_statement") {
    // "from X import a, b": one edge for the module, as written (relative
    // imports keep their dots).
    const TSNode module = detail::field(node, "module_name");
    if (!ts_node_is_null(module)) {
      ex.graph->includes.push_back(IncludeEdge{ex.path,
                                               detail::node_text(ex.source, module),
                                               false, detail::node_line(node)});
    }
    return;
  }
  // "import a.b as c, d": one edge per imported module.
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode child = ts_node_named_child(node, i);
    const std::string_view child_type = ts_node_type(child);
    TSNode module = child;
    if (child_type == "aliased_import") {
      module = detail::field(child, "name");
    } else if (child_type != "dotted_name") {
      continue;
    }
    if (ts_node_is_null(module)) {
      continue;
    }
    ex.graph->includes.push_back(IncludeEdge{ex.path,
                                             detail::node_text(ex.source, module),
                                             false, detail::node_line(node)});
  }
}

void extract_py_call(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  const TSNode function = detail::field(node, "function");
  if (ts_node_is_null(function)) {
    return;
  }
  const std::string_view type = ts_node_type(function);
  TSNode callee = function;
  if (type == "attribute") {
    callee = detail::field(function, "attribute");  // obj.method() -> method
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

void visit_py(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "function_definition") {
    extract_py_function(node, ex);
    return;
  }
  if (type == "class_definition") {
    extract_py_class(node, ex);
    return;
  }
  if (!ex.current_caller.empty()) {
    if (type == "call") {
      extract_py_call(node, ex);
    }
  } else if (type == "assignment") {
    extract_py_assignment(node, ex);
  } else if (type == "import_statement" || type == "import_from_statement") {
    extract_py_import(node, ex);
  }
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit_py(ts_node_named_child(node, i), ex);
  }
}

void extract_python(TSNode root, Extraction& ex) { visit_py(root, ex); }

}  // namespace

std::unique_ptr<LanguageBackend> make_python_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_python(), "python", std::vector<std::string>{".py"},
      extract_python);
}

}  // namespace cf::codeintel
