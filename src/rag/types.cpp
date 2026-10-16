#include "cf/rag/types.hpp"

namespace cf::rag {

std::string_view to_string(DocType type) {
  switch (type) {
    case DocType::Policy: return "policy";
    case DocType::Requirements: return "requirements";
    case DocType::Architecture: return "architecture";
    case DocType::Design: return "design";
    case DocType::Guideline: return "guideline";
    case DocType::Pattern: return "pattern";
    case DocType::ExternalReference: return "external_reference";
  }
  return "unknown";
}

std::optional<DocType> doc_type_from_string(std::string_view text) {
  for (const DocType type :
       {DocType::Policy, DocType::Requirements, DocType::Architecture, DocType::Design,
        DocType::Guideline, DocType::Pattern, DocType::ExternalReference}) {
    if (to_string(type) == text) {
      return type;
    }
  }
  return std::nullopt;
}

int precedence_rank(DocType type) {
  switch (type) {
    case DocType::Policy: return 0;
    case DocType::Requirements: return 1;
    case DocType::Architecture: return 2;
    case DocType::Design: return 3;
    case DocType::Guideline: return 4;
    case DocType::Pattern: return 5;
    case DocType::ExternalReference: return 6;
  }
  return 7;
}

int scope_specificity(std::string_view scope) {
  if (scope == "component") {
    return 3;
  }
  if (scope == "product" || scope == "language") {
    return 2;
  }
  return 0;  // company, or unknown scopes
}

}  // namespace cf::rag
