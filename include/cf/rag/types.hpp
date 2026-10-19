// cf-rag: retrieval engine types (design section 6.3).
//
// Documents carry the metadata the applicability filter and the precedence
// rules need: type, scope, effective dates, status and classification.
// Chunks are the retrieval unit; a chunk inherits its document's metadata.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::rag {

using UnixTime = int64_t;

// Document types in precedence order (design section 6.3); the rank function
// below fixes the order: policy first, external reference last.
enum class DocType {
  Policy,
  Requirements,
  Architecture,
  Design,
  Guideline,
  Pattern,
  ExternalReference,
};

std::string_view to_string(DocType type);
std::optional<DocType> doc_type_from_string(std::string_view text);

// Lower rank = higher precedence.
int precedence_rank(DocType type);

// component 3, product 2, language 2, company 0 (design section 6.3 step 3).
int scope_specificity(std::string_view scope);

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kDuplicateDocument = "RAG_DUPLICATE_DOCUMENT";
inline constexpr const char* kDimensionMismatch = "RAG_DIMENSION_MISMATCH";
inline constexpr const char* kNotFound = "RAG_NOT_FOUND";
inline constexpr const char* kEmptyDocument = "RAG_EMPTY_DOCUMENT";
inline constexpr const char* kInvalidDocument = "RAG_INVALID_DOCUMENT";
inline constexpr const char* kCatalogParseFailed = "RAG_CATALOG_PARSE_FAILED";
}  // namespace error_codes

struct Document {
  std::string doc_id;
  std::string version;
  DocType type = DocType::Guideline;
  std::string owner;
  std::string classification = "internal";  // public | internal | restricted
  std::string scope = "company";            // company | product | component | language
  std::string scope_value;                  // product/component name or language
  UnixTime effective_from = 0;
  UnixTime effective_to = 0;  // 0 = open-ended
  std::string status = "active";  // active | draft | superseded | withdrawn
  // Conflict-group key (design section 6.3): chunks that share a topic are
  // compared by the precedence rules and unresolved conflicts are flagged.
  std::string topic;
  std::string source;             // citation base, e.g. a repo path or URL
  std::string license;            // e.g. CC-BY-SA-3.0 (reference mirror)
  std::string text;               // full document text
};

struct Chunk {
  std::string chunk_id;
  std::string doc_id;
  std::string version;
  std::string text;
  std::string citation;
  DocType type = DocType::Guideline;
  // Optional conflict-group key: chunks that share a topic (a rule ID such as
  // CG-4.2, or a free-text topic) are compared by the precedence rules and
  // resolved into one winner (design section 6.3).
  std::string topic;
  // Applicability metadata copied from the document.
  std::string scope;
  std::string scope_value;
  UnixTime effective_from = 0;
  UnixTime effective_to = 0;
  std::string status;
};

// The run's applicability context (design section 6.3 step 1).
struct QueryContext {
  std::string product;
  std::string component;
  std::string language;
  UnixTime as_of = 0;  // 0 = no date filter
};

struct Query {
  std::string text;
  std::vector<DocType> types;  // empty = all types
  uint32_t top_k = 8;
  QueryContext context;
};

struct Hit {
  std::string chunk_id;
  std::string doc_id;
  std::string version;
  std::string text;
  std::string citation;
  DocType type = DocType::Guideline;
  float score = 0.0f;
  // Set on the winner of a conflict group; overridden_by lists the losing
  // chunk ids and deciding_rule says which rule settled it.
  bool conflict = false;
  std::vector<std::string> overridden_by;
  std::string deciding_rule;
  // Set when the conflict must not be resolved automatically (design section
  // 6.3 step 4): two requirements, or a requirement against a policy.
  bool unresolved_conflict = false;
};

}  // namespace cf::rag
