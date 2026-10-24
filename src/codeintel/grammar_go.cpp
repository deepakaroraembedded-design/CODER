// cf-codeintel: the Go grammar extractor (tree-sitter-go, design section
// 6.5).
//
// Records functions and methods (qualified "Receiver.Method"), type
// declarations (structs, interfaces, aliases), package-level vars and
// consts, imports as include edges and call edges with caller attribution
// (selector calls pkg.Fn record Fn). Doc comments follow the Go convention:
// the comment immediately preceding the declaration.
#include <string>
#include <string_view>

#include "cf/codeintel/treesitter_backend.hpp"

#include "ts_util.hpp"

// The vendored Go grammar (third_party/tree-sitter-go, see VERSIONS).
extern "C" const TSLanguage* tree_sitter_go(void);

namespace cf::codeintel {
namespace {

using detail::Extraction;

// The receiver type name of a method declaration: the first
// parameter_declaration's type_identifier (through pointer_type).
std::string receiver_type(std::string_view source, TSNode receiver) {
  const std::uint32_t count = ts_node_named_child_count(receiver);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode parameter = ts_node_named_child(receiver, i);
    if (std::string_view(ts_node_type(parameter)) != "parameter_declaration") {
      continue;
    }
    TSNode type = detail::field(parameter, "type");
    while (!ts_node_is_null(type)) {
      const std::string_view kind = ts_node_type(type);
      if (kind == "type_identifier") {
        return detail::node_text(source, type);
      }
      if (kind == "pointer_type") {
        type = ts_node_named_child(type, 0);
        continue;
      }
      break;
    }
  }
  return {};
}

void visit_go(TSNode node, Extraction& ex);

// A function or method declaration: the symbol, then the body walked for
// calls (defer and go statements hold plain call_expressions).
void extract_go_callable(TSNode node, Extraction& ex, bool method) {
  const TSNode name = detail::field(node, "name");
  if (ts_node_is_null(name)) {
    return;
  }
  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.kind = method ? SymbolKind::Method : SymbolKind::Function;
  if (method) {
    const std::string receiver =
        receiver_type(ex.source, detail::field(node, "receiver"));
    symbol.qualified = receiver.empty() ? symbol.name
                                        : receiver + "." + symbol.name;
  } else {
    symbol.qualified = symbol.name;
  }
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  symbol.doc = detail::doc_comment(ex.source, node);
  const TSNode body = detail::field(node, "body");
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_is_null(body) ? ts_node_end_byte(node)
                                                  : ts_node_start_byte(body);
  symbol.signature = detail::rtrim(std::string(ex.source.substr(begin, end - begin)));
  const std::string caller = symbol.qualified;  // before the move
  ex.graph->symbols.push_back(std::move(symbol));

  const std::string previous_caller = std::move(ex.current_caller);
  ex.current_caller = caller;
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit_go(ts_node_named_child(node, i), ex);
  }
  ex.current_caller = previous_caller;
}

// A type_spec or type_alias inside a type_declaration.
void extract_go_type(TSNode node, Extraction& ex, bool alias) {
  const TSNode name = detail::field(node, "name");
  if (ts_node_is_null(name)) {
    return;
  }
  const TSNode type = detail::field(node, "type");
  const std::string_view type_kind =
      ts_node_is_null(type) ? "" : ts_node_type(type);

  Symbol symbol;
  symbol.name = detail::node_text(ex.source, name);
  symbol.qualified = symbol.name;
  if (!alias && type_kind == "struct_type") {
    symbol.kind = SymbolKind::Struct;
    symbol.signature = "type " + symbol.name + " struct";
  } else if (!alias && type_kind == "interface_type") {
    symbol.kind = SymbolKind::Interface;
    symbol.signature = "type " + symbol.name + " interface";
  } else {
    symbol.kind = SymbolKind::Typedef;
    symbol.signature = detail::rtrim(detail::node_text(ex.source, node));
  }
  symbol.file = ex.path;
  symbol.line = detail::node_line(node);
  symbol.is_definition = true;
  // The doc comment may precede the spec (grouped declarations) or, when
  // the declaration holds a single spec, the whole type_declaration.
  symbol.doc = detail::doc_comment(ex.source, node);
  if (symbol.doc.empty()) {
    const TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent) &&
        std::string_view(ts_node_type(parent)) == "type_declaration" &&
        ts_node_named_child_count(parent) == 1) {  // a single-spec declaration
      symbol.doc = detail::doc_comment(ex.source, parent);
    }
  }
  ex.graph->symbols.push_back(std::move(symbol));
}

// Package-level var/const declarations: one symbol per declared name.
void extract_go_values(TSNode node, Extraction& ex) {
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode spec = ts_node_named_child(node, i);
    const std::string_view spec_type = ts_node_type(spec);
    if (spec_type != "var_spec" && spec_type != "const_spec") {
      continue;
    }
    const std::uint32_t names = ts_node_named_child_count(spec);
    for (std::uint32_t j = 0; j < names; ++j) {
      const TSNode child = ts_node_named_child(spec, j);
      if (std::string_view(ts_node_type(child)) != "identifier") {
        continue;
      }
      Symbol symbol;
      symbol.name = detail::node_text(ex.source, child);
      symbol.qualified = symbol.name;
      symbol.kind = SymbolKind::Variable;
      symbol.file = ex.path;
      symbol.line = detail::node_line(child);
      symbol.is_definition = true;
      symbol.signature = detail::rtrim(detail::node_text(ex.source, spec));
      ex.graph->symbols.push_back(std::move(symbol));
    }
  }
}

void extract_go_import(TSNode node, Extraction& ex) {
  // import_declaration children: import_spec or import_spec_list (of specs).
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode child = ts_node_named_child(node, i);
    const std::string_view type = ts_node_type(child);
    if (type == "import_spec_list") {
      const std::uint32_t specs = ts_node_named_child_count(child);
      for (std::uint32_t j = 0; j < specs; ++j) {
        const TSNode spec = ts_node_named_child(child, j);
        if (std::string_view(ts_node_type(spec)) != "import_spec") {
          continue;
        }
        const TSNode path = detail::field(spec, "path");
        if (!ts_node_is_null(path)) {
          std::string text = detail::node_text(ex.source, path);
          if (text.size() >= 2) {
            text = text.substr(1, text.size() - 2);  // strip the quotes
          }
          ex.graph->includes.push_back(IncludeEdge{
              ex.path, std::move(text), false, detail::node_line(spec)});
        }
      }
    } else if (type == "import_spec") {
      const TSNode path = detail::field(child, "path");
      if (!ts_node_is_null(path)) {
        std::string text = detail::node_text(ex.source, path);
        if (text.size() >= 2) {
          text = text.substr(1, text.size() - 2);
        }
        ex.graph->includes.push_back(IncludeEdge{ex.path, std::move(text), false,
                                                 detail::node_line(child)});
      }
    }
  }
}

void extract_go_call(TSNode node, Extraction& ex) {
  if (ex.current_caller.empty()) {
    return;
  }
  const TSNode function = detail::field(node, "function");
  if (ts_node_is_null(function)) {
    return;
  }
  const std::string_view type = ts_node_type(function);
  TSNode callee = function;
  if (type == "selector_expression") {
    callee = detail::field(function, "field");  // pkg.Fn() / s.Fn() -> Fn
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

void visit_go(TSNode node, Extraction& ex) {
  const std::string_view type = ts_node_type(node);
  if (type == "function_declaration") {
    extract_go_callable(node, ex, /*method=*/false);
    return;
  }
  if (type == "method_declaration") {
    extract_go_callable(node, ex, /*method=*/true);
    return;
  }
  if (ex.current_caller.empty()) {
    if (type == "type_declaration") {
      const std::uint32_t count = ts_node_named_child_count(node);
      for (std::uint32_t i = 0; i < count; ++i) {
        const TSNode child = ts_node_named_child(node, i);
        const std::string_view child_type = ts_node_type(child);
        if (child_type == "type_spec") {
          extract_go_type(child, ex, /*alias=*/false);
        } else if (child_type == "type_alias") {
          extract_go_type(child, ex, /*alias=*/true);
        }
      }
      return;  // type declarations have no callable content
    }
    if (type == "var_declaration" || type == "const_declaration") {
      extract_go_values(node, ex);
    } else if (type == "import_declaration") {
      extract_go_import(node, ex);
    }
  } else if (type == "call_expression") {
    extract_go_call(node, ex);
  }
  const std::uint32_t count = ts_node_named_child_count(node);
  for (std::uint32_t i = 0; i < count; ++i) {
    visit_go(ts_node_named_child(node, i), ex);
  }
}

void extract_go(TSNode root, Extraction& ex) { visit_go(root, ex); }

}  // namespace

std::unique_ptr<LanguageBackend> make_go_backend() {
  return std::make_unique<TreeSitterBackend>(
      tree_sitter_go(), "go", std::vector<std::string>{".go"}, extract_go);
}

}  // namespace cf::codeintel
