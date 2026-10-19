#include "cf/rag/evaluation.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace cf::rag {
namespace {

Result<RetrievalEvalSet> validate(RetrievalEvalSet set) {
  if (set.collection.empty()) {
    return fail(error_codes::kEvalSetInvalid, "the evaluation set needs a collection");
  }
  if (set.k < 1) {
    return fail(error_codes::kEvalSetInvalid, "k must be at least 1");
  }
  if (set.queries.empty()) {
    return fail(error_codes::kEvalSetInvalid, "the evaluation set has no queries");
  }
  for (const auto& labeled : set.queries) {
    if (labeled.query.empty()) {
      return fail(error_codes::kEvalSetInvalid, "every query needs text");
    }
    if (labeled.relevant_chunks.empty() && labeled.relevant_docs.empty() &&
        !labeled.expect_conflict && !labeled.expect_unresolved_conflict) {
      return fail(error_codes::kEvalSetInvalid,
                  "query '" + labeled.query + "' has no relevant chunks or documents");
    }
  }
  return set;
}

}  // namespace

Result<RetrievalEvalSet> parse_eval_set(std::string_view yaml,
                                        const std::filesystem::path& base_dir) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(yaml));
  } catch (const YAML::Exception& error) {
    return fail(error_codes::kEvalSetInvalid, std::string("eval set yaml: ") + error.what());
  }
  if (!root.IsMap()) {
    return fail(error_codes::kEvalSetInvalid, "the evaluation set must be a mapping");
  }

  RetrievalEvalSet set;
  set.collection = root["collection"].as<std::string>("");
  set.k = root["k"].as<std::size_t>(8);
  const std::string docs = root["docs"].as<std::string>("");
  if (!docs.empty()) {
    set.docs_dir = base_dir / docs;
  }
  const auto queries = root["queries"];
  if (!queries || !queries.IsSequence()) {
    return fail(error_codes::kEvalSetInvalid, "the evaluation set needs a 'queries' sequence");
  }
  for (const auto& node : queries) {
    LabeledQuery labeled;
    labeled.query = node["query"].as<std::string>("");
    if (const auto relevant = node["relevant"]) {
      labeled.relevant_chunks = relevant.as<std::vector<std::string>>();
    }
    if (const auto relevant_docs = node["relevant_docs"]) {
      labeled.relevant_docs = relevant_docs.as<std::vector<std::string>>();
    }
    labeled.expect_conflict = node["expect_conflict"].as<bool>(false);
    labeled.expect_unresolved_conflict =
        node["expect_unresolved_conflict"].as<bool>(false);
    labeled.context.product = node["product"].as<std::string>("");
    labeled.context.component = node["component"].as<std::string>("");
    labeled.context.language = node["language"].as<std::string>("");
    labeled.context.as_of = node["as_of"].as<UnixTime>(0);
    set.queries.push_back(std::move(labeled));
  }
  return validate(std::move(set));
}

Result<RetrievalEvalSet> load_eval_set(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kEvalSetInvalid,
                "cannot read evaluation set: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_eval_set(buffer.str(), path.parent_path());
}

Result<RetrievalMetrics> evaluate_retrieval(const Retriever& retriever,
                                            const RetrievalEvalSet& set) {
  RetrievalMetrics metrics;
  metrics.collection = set.collection;
  metrics.k = set.k;
  metrics.queries = set.queries.size();

  double recall_sum = 0.0;
  double reciprocal_rank_sum = 0.0;
  for (const auto& labeled : set.queries) {
    Query query;
    query.text = labeled.query;
    query.top_k = static_cast<uint32_t>(set.k);
    query.context = labeled.context;
    auto hits = retriever.search(query);
    if (!hits) {
      return tl::make_unexpected(hits.error());
    }
    const std::set<std::string> chunk_labels(labeled.relevant_chunks.begin(),
                                             labeled.relevant_chunks.end());
    const std::set<std::string> doc_labels(labeled.relevant_docs.begin(),
                                           labeled.relevant_docs.end());
    std::set<std::string> matched_chunks;
    std::set<std::string> matched_docs;
    std::size_t first_rank = 0;
    bool saw_conflict = false;
    bool saw_unresolved_conflict = false;
    const std::size_t limit = std::min(hits->size(), set.k);
    for (std::size_t rank = 0; rank < limit; ++rank) {
      const auto& hit = (*hits)[rank];
      const bool chunk_hit = chunk_labels.find(hit.chunk_id) != chunk_labels.end();
      const bool doc_hit = doc_labels.find(hit.doc_id) != doc_labels.end();
      if (chunk_hit) {
        matched_chunks.insert(hit.chunk_id);
      }
      if (doc_hit) {
        matched_docs.insert(hit.doc_id);
      }
      if (first_rank == 0 && (chunk_hit || doc_hit)) {
        first_rank = rank + 1;
      }
      saw_conflict = saw_conflict || hit.conflict;
      saw_unresolved_conflict = saw_unresolved_conflict || hit.unresolved_conflict;
    }
    if (labeled.expect_conflict) {
      ++metrics.conflict_queries;
      if (!saw_conflict) {
        ++metrics.conflict_misses;
      }
    }
    if (labeled.expect_unresolved_conflict) {
      ++metrics.unresolved_conflict_queries;
      if (!saw_unresolved_conflict) {
        ++metrics.unresolved_conflict_misses;
      }
    }
    const double labels = static_cast<double>(chunk_labels.size() + doc_labels.size());
    if (labels > 0.0) {
      recall_sum +=
          static_cast<double>(matched_chunks.size() + matched_docs.size()) / labels;
      reciprocal_rank_sum +=
          first_rank > 0 ? 1.0 / static_cast<double>(first_rank) : 0.0;
      if (first_rank == 0) {
        ++metrics.misses;
      }
      ++metrics.scored_queries;
    }
  }
  const double queries = static_cast<double>(metrics.scored_queries);
  metrics.recall = queries > 0.0 ? recall_sum / queries : 0.0;
  metrics.mrr = queries > 0.0 ? reciprocal_rank_sum / queries : 0.0;
  return metrics;
}

bool meets_retrieval_targets(const RetrievalMetrics& metrics) {
  // A conflict-only set has no relevance labels, so the recall/MRR targets do
  // not apply; the conflict expectations still must all be met.
  const bool retrieval_ok =
      metrics.scored_queries == 0 ||
      (metrics.recall >= kRecallTarget && metrics.mrr >= kMrrTarget);
  return retrieval_ok && metrics.conflict_misses == 0 &&
         metrics.unresolved_conflict_misses == 0;
}

}  // namespace cf::rag
