#include "cf/codeintel/context_packer.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace cf::codeintel {
namespace {

std::uint32_t div_round_up(std::size_t value, std::size_t divisor) {
  return static_cast<std::uint32_t>((value + divisor - 1) / divisor);
}

std::string describe(const Symbol& symbol) {
  return std::string(to_string(symbol.kind)) + " " + symbol.qualified + " (" +
         symbol.file + ":" + std::to_string(symbol.line) + ")";
}

// One symbol rendered for the packed block: a header line, then the
// signature and the doc comment indented under it.
std::string render(const Symbol& symbol) {
  std::string block = describe(symbol) + "\n";
  if (!symbol.signature.empty()) {
    block += "  " + symbol.signature + "\n";
  }
  if (!symbol.doc.empty()) {
    block += "  " + symbol.doc + "\n";
  }
  return block;
}

// A call site whose far side is not in the index (external calls like
// printf) still earns a one-line mention.
std::string render_site(const CallEdge& edge, bool caller_known) {
  if (caller_known) {
    return "called from " + edge.caller + " (" + edge.file + ":" +
           std::to_string(edge.line) + ")\n";
  }
  return "calls " + edge.callee + " (" + edge.file + ":" +
         std::to_string(edge.line) + ")\n";
}

// Accumulates rendered sections under the token budget (design section 6.5,
// "all within a token budget"): entries are ranked by the order they are
// offered, and an entry that does not fit is recorded as dropped. Accounting
// is in characters (~4 per token), so the final estimate is exact.
struct Pack {
  explicit Pack(std::uint32_t token_budget) : budget_chars(token_budget * 4) {}

  bool offer(const Symbol& symbol, std::set<std::string>& seen) {
    const std::string key =
        symbol.file + ":" + std::to_string(symbol.line) + ":" + symbol.qualified;
    if (!seen.insert(key).second) {
      return true;  // already packed
    }
    const std::string block = render(symbol);
    if (chars + block.size() > budget_chars) {
      dropped.push_back(describe(symbol));
      return false;
    }
    chars += block.size();
    included.push_back(describe(symbol));
    text += block;
    return true;
  }

  void offer_site(const CallEdge& edge, bool caller_known) {
    const std::string line = render_site(edge, caller_known);
    if (chars + line.size() > budget_chars) {
      return;  // one-line notes are dropped silently when the budget is out
    }
    chars += line.size();
    text += line;
  }

  std::string text;
  std::size_t chars = 0;
  std::size_t budget_chars;
  std::vector<std::string> included;
  std::vector<std::string> dropped;
};

}  // namespace

std::uint32_t estimate_tokens(std::string_view text) {
  return div_round_up(text.size(), 4);
}

Result<PackedContext> ContextPacker::pack(const ContextRequest& request) const {
  // 1. Resolve the seeds: symbol names first, then file/path prefixes.
  std::vector<Symbol> seeds;
  PackedContext result;
  for (const auto& seed : request.seeds) {
    auto by_name = index_->find_symbol(seed);
    if (!by_name) {
      return tl::make_unexpected(by_name.error());
    }
    if (!by_name->empty()) {
      seeds.insert(seeds.end(), by_name->begin(), by_name->end());
      continue;
    }
    auto by_path = index_->symbols_under(seed);
    if (!by_path) {
      return tl::make_unexpected(by_path.error());
    }
    if (!by_path->empty()) {
      seeds.insert(seeds.end(), by_path->begin(), by_path->end());
      continue;
    }
    result.unresolved.push_back(seed);
  }

  Pack packer(request.token_budget);
  std::set<std::string> seen;

  // 2. The seed symbols themselves (the target component's surface).
  for (const auto& symbol : seeds) {
    packer.offer(symbol, seen);
  }

  // 3. Direct dependencies: the signatures of what the seeds call.
  for (const auto& seed : seeds) {
    auto callees = index_->get_callees(seed.name);
    if (!callees) {
      return tl::make_unexpected(callees.error());
    }
    for (const auto& edge : *callees) {
      auto resolved = index_->find_symbol(edge.callee);
      if (!resolved) {
        return tl::make_unexpected(resolved.error());
      }
      if (resolved->empty()) {
        packer.offer_site(edge, /*caller_known=*/false);
        continue;
      }
      packer.offer(resolved->front(), seen);
    }
  }

  // 4. Callers: the signatures of what calls the seeds.
  for (const auto& seed : seeds) {
    auto callers = index_->get_callers(seed.name);
    if (!callers) {
      return tl::make_unexpected(callers.error());
    }
    for (const auto& edge : *callers) {
      auto resolved = index_->find_symbol(edge.caller);
      if (!resolved) {
        return tl::make_unexpected(resolved.error());
      }
      if (resolved->empty()) {
        packer.offer_site(edge, /*caller_known=*/true);
        continue;
      }
      packer.offer(resolved->front(), seen);
    }
  }

  // 5. The rest of the seed files (the component's remaining surface).
  std::set<std::string> seed_files;
  for (const auto& seed : seeds) {
    seed_files.insert(seed.file);
  }
  for (const auto& file : seed_files) {
    auto symbols = index_->symbols_under(file);
    if (!symbols) {
      return tl::make_unexpected(symbols.error());
    }
    for (const auto& symbol : *symbols) {
      packer.offer(symbol, seen);
    }
  }

  result.text = std::move(packer.text);
  result.tokens = estimate_tokens(result.text);
  result.included = std::move(packer.included);
  result.dropped = std::move(packer.dropped);
  return result;
}

}  // namespace cf::codeintel
