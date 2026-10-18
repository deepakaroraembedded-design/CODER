// cf-rag: hybrid retriever with applicability and precedence (design section
// 6.3).
//
// Query path: BM25 and vector retrieval, combined with Reciprocal Rank
// Fusion, then the applicability filter and precedence resolution, returning
// top-k hits with citations. Chunks that share a topic are resolved into one
// winner flagged `conflict` with the losing chunks under `overridden_by`;
// conflicts that must never be resolved automatically (requirement against
// requirement or policy) are flagged `unresolved_conflict` for escalation.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/rag/bm25.hpp"
#include "cf/rag/chunker.hpp"
#include "cf/rag/embedder.hpp"
#include "cf/rag/types.hpp"
#include "cf/rag/vector_index.hpp"

namespace cf::rag {

struct RetrieverOptions {
  ChunkOptions chunking;
  double bm25_k1 = 1.2;
  double bm25_b = 0.75;
  std::size_t rrf_k = 60;               // Reciprocal Rank Fusion constant
  std::size_t candidate_multiplier = 3; // candidates per ranking list
  // hnswlib is the default vector index (design section 6.3); false uses the
  // exact brute-force index (the parity reference and small collections).
  bool hnsw = true;
};

class Retriever {
 public:
  virtual ~Retriever() = default;

  virtual Result<std::vector<Hit>> search(const Query& query) const = 0;

  virtual Result<Document> get(std::string_view doc_id,
                               std::string_view version) const = 0;
};

class InMemoryRetriever final : public Retriever {
 public:
  explicit InMemoryRetriever(RetrieverOptions options = {});

  // When set, documents are embedded at ingestion and queries are embedded
  // for the vector half of the hybrid retrieval.
  void set_embedder(std::shared_ptr<Embedder> embedder);

  // Chunks the document (Markdown-aware), embeds it when an embedder is set,
  // and indexes it. A (doc_id, version) pair can only be ingested once.
  Status add_document(Document document);

  // Adds a pre-chunked piece (code chunks, tests, or documents already
  // chunked elsewhere). The chunk is embedded when an embedder is set.
  Status add_chunk(Chunk chunk);

  Result<std::vector<Hit>> search(const Query& query) const override;

  // An empty version returns the newest ingested version of the document.
  Result<Document> get(std::string_view doc_id, std::string_view version) const override;

  std::size_t chunk_count() const { return chunks_.size(); }

 private:
  // Applicability filter (design section 6.3 step 1): active status, matching
  // scope and the run's as-of date.
  static bool applies(const Chunk& chunk, const QueryContext& context);

  Status index_chunk(const Chunk& chunk, const std::vector<float>* embedding);

  RetrieverOptions options_;
  std::shared_ptr<Embedder> embedder_;
  Bm25Index bm25_;
  std::unique_ptr<VectorIndex> vector_index_;
  std::map<std::string, Document, std::less<>> documents_;  // "doc_id@version"
  std::map<std::string, Chunk, std::less<>> chunks_;        // chunk_id
};

}  // namespace cf::rag
