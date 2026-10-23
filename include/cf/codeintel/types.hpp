// cf-codeintel: code intelligence (design section 6.5).
//
// The types every language back end produces: a symbol graph (symbols, call
// edges and include edges over the parsed files), diagnostics from the
// guardrail/architecture checks, and the module's error-code registry
// (design section 4.1).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::codeintel {

// Stable error codes (design section 4.1: one registry per module).
namespace error_codes {
inline constexpr const char* kUnknownLanguage = "CODEINTEL_UNKNOWN_LANGUAGE";
inline constexpr const char* kParseFailed = "CODEINTEL_PARSE_FAILED";
inline constexpr const char* kIndexOpenFailed = "CODEINTEL_INDEX_OPEN_FAILED";
inline constexpr const char* kIndexQueryFailed = "CODEINTEL_INDEX_QUERY_FAILED";
}  // namespace error_codes

enum class SymbolKind {
  Function,
  Method,
  Class,
  Struct,
  Enum,
  Typedef,
  Variable,
  Macro,
  Namespace,
};

std::string_view to_string(SymbolKind kind);
std::optional<SymbolKind> symbol_kind_from_string(std::string_view text);

struct Symbol {
  std::string name;       // unqualified, e.g. "parse_header"
  std::string qualified;  // e.g. "cf::parse_header"; equals name when the
                          // language has no namespaces
  SymbolKind kind = SymbolKind::Function;
  std::string file;        // path relative to the indexed root
  std::uint32_t line = 0;  // 1-based
  std::string signature;   // declaration text without the body
  std::string doc;         // leading doc comment, when the back end extracts it
  bool is_definition = false;
};

struct CallEdge {
  std::string caller;      // name of the enclosing function
  std::string callee;      // the called function's name as written
  std::string file;        // path relative to the indexed root
  std::uint32_t line = 0;  // 1-based
};

struct IncludeEdge {
  std::string file;      // the file containing the include
  std::string included;  // the include path as written, without <> or quotes
  bool system = false;   // true for <...>, false for "..."
  std::uint32_t line = 0;
};

// One parsed file and its content hash; the hash drives incremental updates
// (design section 6.5, "updated incrementally per commit").
struct FileRecord {
  std::string path;  // relative to the indexed root
  std::string sha256;
};

struct SymbolGraph {
  std::string root;      // the indexed root directory
  std::string language;  // the producing back end's language id
  std::vector<Symbol> symbols;
  std::vector<CallEdge> calls;
  std::vector<IncludeEdge> includes;
  std::vector<FileRecord> files;
};

struct Diagnostic {
  std::string rule_id;  // e.g. "CG-4.2" or the architecture rule's id
  std::string message;
  std::string file;
  std::uint32_t line = 0;
};

// Layering rule (design section 6.5, "Architecture rules"): files under
// `from_prefix` must not include files whose path starts with
// `forbidden_prefix`. Paths are the index's root-relative paths; the include
// is matched as written in the source.
struct LayerRule {
  std::string rule_id;  // reported on the diagnostic
  std::string from_prefix;
  std::string forbidden_prefix;
};

struct ArchitectureRules {
  std::vector<LayerRule> layers;
};

}  // namespace cf::codeintel
