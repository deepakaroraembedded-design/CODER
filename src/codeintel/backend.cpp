#include "cf/codeintel/backend.hpp"

#include <utility>

namespace cf::codeintel {

void BackendRegistry::add(std::unique_ptr<LanguageBackend> backend) {
  backends_.push_back(std::move(backend));
}

Result<LanguageBackend*> BackendRegistry::for_language(std::string_view language) const {
  for (const auto& backend : backends_) {
    if (backend->language() == language) {
      return backend.get();
    }
  }
  return fail(error_codes::kUnknownLanguage,
              "no code intelligence back end for language '" + std::string(language) + "'");
}

std::vector<Diagnostic> check_layer_rules(const SymbolGraph& graph,
                                          const ArchitectureRules& rules) {
  std::vector<Diagnostic> diagnostics;
  for (const auto& include : graph.includes) {
    if (include.system) {
      continue;  // system includes are not project paths
    }
    for (const auto& rule : rules.layers) {
      const bool in_layer = include.file.compare(0, rule.from_prefix.size(),
                                                 rule.from_prefix) == 0;
      const bool forbidden = include.included.compare(0, rule.forbidden_prefix.size(),
                                                      rule.forbidden_prefix) == 0;
      if (in_layer && forbidden) {
        diagnostics.push_back(Diagnostic{
            rule.rule_id,
            include.file + " must not include " + include.included + " (" +
                rule.from_prefix + " is independent of " + rule.forbidden_prefix + ")",
            include.file, include.line});
      }
    }
  }
  return diagnostics;
}

}  // namespace cf::codeintel
