#include "cf/rag/retriever.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace cf::rag {
namespace {

bool is_requirement_conflict(DocType winner, DocType loser) {
  // Design section 6.3 step 4: never resolved automatically between two
  // requirements, or between a requirement and a policy.
  if (winner == DocType::Requirements &&
      (loser == DocType::Requirements || loser == DocType::Policy)) {
    return true;
  }
  if (winner == DocType::Policy && loser == DocType::Requirements) {
    return true;
  }
  return false;
}

// The configured precedence list overrides the built-in type order; types it
// does not name keep their relative built-in order after the listed ones.
int ranked_precedence(DocType type, const std::vector<DocType>& order) {
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (order[i] == type) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(order.size()) + precedence_rank(type);
}

// Order used to pick a conflict-group winner: precedence first, then the more
// specific scope, then the later effective date, then the retrieval score.
bool beats(const Chunk& a, double score_a, const Chunk& b, double score_b,
           const std::vector<DocType>& precedence) {
  const int rank_a = ranked_precedence(a.type, precedence);
  const int rank_b = ranked_precedence(b.type, precedence);
  if (rank_a != rank_b) {
    return rank_a < rank_b;
  }
  const int scope_a = scope_specificity(a.scope);
  const int scope_b = scope_specificity(b.scope);
  if (scope_a != scope_b) {
    return scope_a > scope_b;
  }
  if (a.effective_from != b.effective_from) {
    return a.effective_from > b.effective_from;
  }
  if (score_a != score_b) {
    return score_a > score_b;
  }
  return a.chunk_id < b.chunk_id;  // deterministic
}

std::string deciding_rule(const Chunk& winner, const Chunk& loser,
                          const std::vector<DocType>& precedence) {
  if (ranked_precedence(winner.type, precedence) !=
      ranked_precedence(loser.type, precedence)) {
    return "precedence: " + std::string(to_string(winner.type)) + " over " +
           std::string(to_string(loser.type));
  }
  if (scope_specificity(winner.scope) != scope_specificity(loser.scope)) {
    return "scope: " + winner.scope + " over " + loser.scope;
  }
  if (winner.effective_from != loser.effective_from) {
    return "effective date: newer wins";
  }
  return "relevance: higher retrieval score";
}

}  // namespace

InMemoryRetriever::InMemoryRetriever(RetrieverOptions options)
    : options_(std::move(options)),
      bm25_(options_.bm25_k1, options_.bm25_b) {}

void InMemoryRetriever::set_embedder(std::shared_ptr<Embedder> embedder) {
  embedder_ = std::move(embedder);
  if (embedder_ && vector_index_ == nullptr) {
#ifdef CF_HAS_FAISS
    if (options_.faiss_ivfpq) {
      vector_index_ = make_faiss_ivfpq_vector_index();
    } else if (options_.faiss) {
      vector_index_ = make_faiss_vector_index();
    }
#endif
    if (vector_index_ == nullptr) {
      vector_index_ = options_.hnsw ? std::unique_ptr<VectorIndex>(
                                          std::make_unique<HnswVectorIndex>())
                                    : std::unique_ptr<VectorIndex>(
                                          std::make_unique<BruteForceVectorIndex>());
    }
  }
}

bool InMemoryRetriever::applies(const Chunk& chunk, const QueryContext& context) {
  // Active documents are the default; approved design packages (design
  // section 6.2 step 7) are retrievable too, while superseded, withdrawn and
  // draft documents are not.
  if (chunk.status != "active" && chunk.status != "approved") {
    return false;
  }
  if (chunk.scope == "product") {
    if (context.product.empty() || chunk.scope_value != context.product) {
      return false;
    }
  } else if (chunk.scope == "component") {
    if (context.component.empty() || chunk.scope_value != context.component) {
      return false;
    }
  } else if (chunk.scope == "language") {
    if (context.language.empty() || chunk.scope_value != context.language) {
      return false;
    }
  } else if (chunk.scope != "company") {
    return false;  // unknown scope: not applicable
  }
  if (context.as_of != 0) {
    if (chunk.effective_from != 0 && chunk.effective_from > context.as_of) {
      return false;
    }
    if (chunk.effective_to != 0 && chunk.effective_to < context.as_of) {
      return false;
    }
  }
  return true;
}

Status InMemoryRetriever::index_chunk(const Chunk& chunk,
                                      const std::vector<float>* embedding) {
  std::string chunk_id = chunk.chunk_id;
  int suffix = 2;
  while (chunks_.find(chunk_id) != chunks_.end()) {
    chunk_id = chunk.chunk_id + "-" + std::to_string(suffix++);
  }
  Chunk stored = chunk;
  stored.chunk_id = chunk_id;
  bm25_.add(chunk_id, stored.text);
  if (vector_index_ != nullptr) {
    if (embedding == nullptr) {
      return fail(error_codes::kDimensionMismatch,
                  "embedder is set but no embedding was produced for " + chunk_id);
    }
    auto added = vector_index_->add(chunk_id, *embedding);
    if (!added) {
      return added;
    }
  }
  chunks_.emplace(chunk_id, std::move(stored));
  return {};
}

Status InMemoryRetriever::add_document(Document document) {
  if (document.doc_id.empty() || document.version.empty() || document.text.empty()) {
    return fail(error_codes::kEmptyDocument,
                "document needs a doc_id, a version and text");
  }
  const std::string key = document.doc_id + "@" + document.version;
  if (documents_.find(key) != documents_.end()) {
    return fail(error_codes::kDuplicateDocument,
                "document already ingested: " + key);
  }

  const std::vector<TextChunk> text_chunks =
      chunk_markdown(document.text, options_.chunking);
  if (text_chunks.empty()) {
    return fail(error_codes::kEmptyDocument, "document produced no chunks: " + key);
  }

  std::vector<std::vector<float>> embeddings;
  if (embedder_ != nullptr) {
    std::vector<std::string> texts;
    texts.reserve(text_chunks.size());
    for (const auto& text_chunk : text_chunks) {
      texts.push_back(text_chunk.text);
    }
    auto embedded = embedder_->embed(texts);
    if (!embedded) {
      return tl::make_unexpected(embedded.error());
    }
    if (embedded->size() != text_chunks.size()) {
      return fail(error_codes::kDimensionMismatch,
                  "embedder returned " + std::to_string(embedded->size()) +
                      " vectors for " + std::to_string(text_chunks.size()) + " chunks");
    }
    embeddings = std::move(*embedded);
  }

  const std::string citation_base =
      document.source.empty() ? document.doc_id : document.source;
  for (std::size_t i = 0; i < text_chunks.size(); ++i) {
    Chunk chunk;
    chunk.chunk_id = key + "#" + text_chunks[i].anchor;
    chunk.doc_id = document.doc_id;
    chunk.version = document.version;
    chunk.text = text_chunks[i].text;
    chunk.citation = citation_base + "@" + document.version + "#" + text_chunks[i].anchor;
    chunk.type = document.type;
    chunk.scope = document.scope;
    chunk.scope_value = document.scope_value;
    chunk.effective_from = document.effective_from;
    chunk.effective_to = document.effective_to;
    chunk.status = document.status;
    chunk.topic = document.topic;
    auto indexed = index_chunk(chunk, embeddings.empty() ? nullptr : &embeddings[i]);
    if (!indexed) {
      return indexed;
    }
  }
  documents_.emplace(key, std::move(document));
  return {};
}

Status InMemoryRetriever::add_chunk(Chunk chunk) {
  if (chunk.chunk_id.empty() || chunk.text.empty()) {
    return fail(error_codes::kEmptyDocument, "chunk needs an id and text");
  }
  std::vector<std::vector<float>> embeddings;
  if (embedder_ != nullptr) {
    auto embedded = embedder_->embed({chunk.text});
    if (!embedded) {
      return tl::make_unexpected(embedded.error());
    }
    embeddings = std::move(*embedded);
  }
  return index_chunk(chunk, embeddings.empty() ? nullptr : &embeddings.front());
}

Result<std::vector<Hit>> InMemoryRetriever::search(const Query& query) const {
  struct Latency {
    const std::function<void(std::int64_t)>& observer;
    std::chrono::steady_clock::time_point start;
    ~Latency() {
      if (observer) {
        observer(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count());
      }
    }
  } latency{options_.on_search_latency_ms, std::chrono::steady_clock::now()};

  const std::size_t top_k = std::max<std::size_t>(query.top_k, 1);
  const std::size_t candidates = top_k * options_.candidate_multiplier;

  const auto bm25_ranking = bm25_.search(query.text, candidates);
  std::vector<std::pair<std::string, float>> vector_ranking;
  if (vector_index_ != nullptr && embedder_ != nullptr) {
    auto embedded = embedder_->embed({query.text});
    if (!embedded) {
      return tl::make_unexpected(embedded.error());
    }
    if (embedded->empty()) {
      return fail(error_codes::kDimensionMismatch, "embedder returned no vector");
    }
    vector_ranking = vector_index_->search(embedded->front(), candidates);
  }

  // Weighted Reciprocal Rank Fusion (design sections 6.3, 15).
  std::map<std::string, double, std::less<>> fused;
  for (std::size_t rank = 0; rank < bm25_ranking.size(); ++rank) {
    fused[bm25_ranking[rank].first] +=
        options_.bm25_weight / static_cast<double>(options_.rrf_k + rank + 1);
  }
  for (std::size_t rank = 0; rank < vector_ranking.size(); ++rank) {
    fused[vector_ranking[rank].first] +=
        options_.vector_weight / static_cast<double>(options_.rrf_k + rank + 1);
  }

  struct Candidate {
    const Chunk* chunk;
    double score;
  };
  std::vector<Candidate> applicable;
  for (const auto& [chunk_id, score] : fused) {
    const auto it = chunks_.find(chunk_id);
    if (it == chunks_.end()) {
      continue;
    }
    const Chunk& chunk = it->second;
    if (!query.types.empty() &&
        std::find(query.types.begin(), query.types.end(), chunk.type) ==
            query.types.end()) {
      continue;
    }
    if (!applies(chunk, query.context)) {
      continue;
    }
    applicable.push_back(Candidate{&chunk, score});
  }
  std::sort(applicable.begin(), applicable.end(),
            [](const Candidate& a, const Candidate& b) {
              if (a.score != b.score) {
                return a.score > b.score;
              }
              return a.chunk->chunk_id < b.chunk->chunk_id;
            });

  // Conflict resolution per topic group (design section 6.3).
  std::map<std::string, std::vector<std::size_t>, std::less<>> groups;
  for (std::size_t i = 0; i < applicable.size(); ++i) {
    if (!applicable[i].chunk->topic.empty()) {
      groups[applicable[i].chunk->topic].push_back(i);
    }
  }
  std::vector<bool> dropped(applicable.size(), false);
  std::map<std::size_t, Hit, std::less<>> winners;  // index -> hit
  for (const auto& [topic, members] : groups) {
    (void)topic;
    if (members.size() < 2) {
      continue;
    }
    std::vector<std::size_t> order = members;
    std::sort(order.begin(), order.end(), [&applicable, this](std::size_t a,
                                                             std::size_t b) {
      return beats(*applicable[a].chunk, applicable[a].score,
                   *applicable[b].chunk, applicable[b].score,
                   options_.precedence);
    });
    const std::size_t winner = order.front();
    const Candidate& winner_candidate = applicable[winner];
    Hit hit;
    hit.chunk_id = winner_candidate.chunk->chunk_id;
    hit.doc_id = winner_candidate.chunk->doc_id;
    hit.version = winner_candidate.chunk->version;
    hit.text = winner_candidate.chunk->text;
    hit.citation = winner_candidate.chunk->citation;
    hit.type = winner_candidate.chunk->type;
    hit.score = static_cast<float>(winner_candidate.score);
    hit.conflict = true;
    bool requirement_conflict = false;
    for (const std::size_t loser : order) {
      if (loser == winner) {
        continue;
      }
      hit.overridden_by.push_back(applicable[loser].chunk->chunk_id);
      if (is_requirement_conflict(winner_candidate.chunk->type,
                                  applicable[loser].chunk->type)) {
        requirement_conflict = true;
      }
      dropped[loser] = true;
    }
    const Candidate& runner_up = applicable[order[1]];
    hit.deciding_rule = deciding_rule(*winner_candidate.chunk, *runner_up.chunk,
                                      options_.precedence);
    // Two chunks of the same type (or same precedence), scope and effective
    // date are never resolved automatically (design section 6.3 step 4).
    if (ranked_precedence(winner_candidate.chunk->type, options_.precedence) ==
            ranked_precedence(runner_up.chunk->type, options_.precedence) &&
        scope_specificity(winner_candidate.chunk->scope) ==
            scope_specificity(runner_up.chunk->scope) &&
        winner_candidate.chunk->effective_from == runner_up.chunk->effective_from) {
      hit.unresolved_conflict = true;
      hit.deciding_rule = "unresolved: same type, scope and effective date";
    }
    if (requirement_conflict) {
      hit.unresolved_conflict = true;
      hit.deciding_rule = "unresolved: requirement conflict";
    }
    winners.emplace(winner, std::move(hit));
  }

  std::vector<Hit> hits;
  for (std::size_t i = 0; i < applicable.size(); ++i) {
    if (dropped[i]) {
      continue;
    }
    const auto winner = winners.find(i);
    if (winner != winners.end()) {
      hits.push_back(winner->second);
      continue;
    }
    Hit hit;
    hit.chunk_id = applicable[i].chunk->chunk_id;
    hit.doc_id = applicable[i].chunk->doc_id;
    hit.version = applicable[i].chunk->version;
    hit.text = applicable[i].chunk->text;
    hit.citation = applicable[i].chunk->citation;
    hit.type = applicable[i].chunk->type;
    hit.score = static_cast<float>(applicable[i].score);
    hits.push_back(std::move(hit));
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.chunk_id < b.chunk_id;
  });
  if (hits.size() > top_k) {
    hits.resize(top_k);
  }
  return hits;
}

Result<Document> InMemoryRetriever::get(std::string_view doc_id,
                                        std::string_view version) const {
  if (!version.empty()) {
    const auto it = documents_.find(std::string(doc_id) + "@" + std::string(version));
    if (it == documents_.end()) {
      return fail(error_codes::kNotFound,
                  "document not found: " + std::string(doc_id) + "@" +
                      std::string(version));
    }
    return it->second;
  }
  const Document* newest = nullptr;
  for (const auto& [key, document] : documents_) {
    (void)key;
    if (document.doc_id != doc_id) {
      continue;
    }
    if (newest == nullptr || document.effective_from > newest->effective_from) {
      newest = &document;
    }
  }
  if (newest == nullptr) {
    return fail(error_codes::kNotFound, "document not found: " + std::string(doc_id));
  }
  return *newest;
}

}  // namespace cf::rag
