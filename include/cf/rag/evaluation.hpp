// cf-rag: retrieval evaluation sets and metrics (design section 6.3).
//
// A labeled set lives next to the golden suite in `eval/rag/`: a collection,
// a cutoff k, an optional docs directory, and queries with known relevant
// chunks (`relevant`, chunk ids like `CG@3.2#intro`) and/or relevant documents
// (`relevant_docs`, doc ids — any chunk of the document counts). CI fails when
// recall@k drops below 0.85 or MRR below 0.6.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/rag/retriever.hpp"

namespace cf::rag {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kEvalSetInvalid = "RAG_EVAL_SET_INVALID";
}  // namespace error_codes

struct LabeledQuery {
  std::string query;
  std::vector<std::string> relevant_chunks;  // exact chunk ids
  std::vector<std::string> relevant_docs;    // doc ids: any chunk matches
  // Applicability context (design section 6.3 step 1): product/component/
  // language-scoped chunks only apply when the query carries the matching
  // context, exactly as in a real run.
  QueryContext context;
};

struct RetrievalEvalSet {
  std::string collection;
  std::size_t k = 8;
  std::filesystem::path docs_dir;  // optional; relative to the set file
  std::vector<LabeledQuery> queries;
};

Result<RetrievalEvalSet> parse_eval_set(std::string_view yaml,
                                        const std::filesystem::path& base_dir = {});
Result<RetrievalEvalSet> load_eval_set(const std::filesystem::path& path);

struct RetrievalMetrics {
  std::string collection;
  std::size_t k = 8;
  std::size_t queries = 0;
  std::size_t misses = 0;  // queries with no relevant hit in the top k
  double recall = 0.0;     // mean matched labels / labels per query
  double mrr = 0.0;        // mean reciprocal rank of the first relevant hit
};

Result<RetrievalMetrics> evaluate_retrieval(const Retriever& retriever,
                                            const RetrievalEvalSet& set);

// Design section 6.3: CI fails if recall@8 < 0.85 or MRR < 0.6.
inline constexpr double kRecallTarget = 0.85;
inline constexpr double kMrrTarget = 0.60;
bool meets_retrieval_targets(const RetrievalMetrics& metrics);

}  // namespace cf::rag
