#include "cf/codeintel/types.hpp"

namespace cf::codeintel {

std::string_view to_string(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::Function: return "function";
    case SymbolKind::Method: return "method";
    case SymbolKind::Class: return "class";
    case SymbolKind::Struct: return "struct";
    case SymbolKind::Enum: return "enum";
    case SymbolKind::Typedef: return "typedef";
    case SymbolKind::Variable: return "variable";
    case SymbolKind::Macro: return "macro";
    case SymbolKind::Namespace: return "namespace";
  }
  return "unknown";
}

std::optional<SymbolKind> symbol_kind_from_string(std::string_view text) {
  if (text == "function") return SymbolKind::Function;
  if (text == "method") return SymbolKind::Method;
  if (text == "class") return SymbolKind::Class;
  if (text == "struct") return SymbolKind::Struct;
  if (text == "enum") return SymbolKind::Enum;
  if (text == "typedef") return SymbolKind::Typedef;
  if (text == "variable") return SymbolKind::Variable;
  if (text == "macro") return SymbolKind::Macro;
  if (text == "namespace") return SymbolKind::Namespace;
  return std::nullopt;
}

}  // namespace cf::codeintel
