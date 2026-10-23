// cf-codeintel: the context packer (design section 6.5).
//
// Packs the code context for a task into a bounded block: the seed symbols
// themselves, the signatures of their direct dependencies (what they call),
// their callers' signatures, and the remaining symbols of the seed files —
// ranked in that order and cut to the token budget.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cf/codeintel/symbol_index.hpp"

namespace cf::codeintel {

// A rough token estimate (~4 characters per token); the design's budgets
// are in tokens, and a conservative estimate keeps packs under it.
std::uint32_t estimate_tokens(std::string_view text);

struct ContextRequest {
  // What the task touches: symbol names, or indexed file paths (or path
  // prefixes, e.g. a component directory).
  std::vector<std::string> seeds;
  std::uint32_t token_budget = 4000;
};

struct PackedContext {
  std::string text;                      // the packed block, ready for a prompt
  std::uint32_t tokens = 0;              // estimate_tokens(text)
  std::vector<std::string> included;     // what made it in (audit/logging)
  std::vector<std::string> dropped;      // cut by the budget
  std::vector<std::string> unresolved;   // seeds no symbol or file matched
};

class ContextPacker {
 public:
  explicit ContextPacker(const SymbolIndex& index) : index_(&index) {}

  Result<PackedContext> pack(const ContextRequest& request) const;

 private:
  const SymbolIndex* index_;
};

}  // namespace cf::codeintel
