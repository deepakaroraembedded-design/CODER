// cf-rag: vector index (design section 6.3).
//
// The interface keeps the storage swap-friendly: hnswlib or FAISS replace the
// brute-force implementation without touching the retriever.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::rag {

class VectorIndex {
 public:
  virtual ~VectorIndex() = default;

  // Vectors are normalized on insert; the dimension is fixed by the first
  // vector and later mismatches are rejected.
  virtual Status add(std::string_view chunk_id, std::vector<float> vector) = 0;

  // Cosine similarity (dot product of normalized vectors), best first.
  virtual std::vector<std::pair<std::string, float>> search(
      const std::vector<float>& query, std::size_t limit) const = 0;
};

class BruteForceVectorIndex final : public VectorIndex {
 public:
  Status add(std::string_view chunk_id, std::vector<float> vector) override;

  std::vector<std::pair<std::string, float>> search(
      const std::vector<float>& query, std::size_t limit) const override;

  std::size_t size() const { return entries_.size(); }
  std::size_t dimension() const { return dimension_; }

 private:
  std::size_t dimension_ = 0;
  std::vector<std::pair<std::string, std::vector<float>>> entries_;
};

// Approximate nearest-neighbour index (design section 6.3: hnswlib is the
// default; brute force is the fallback and the parity reference). Vectors are
// normalized on insert, so inner product equals cosine similarity. The index
// grows on demand and concurrent searches are safe once ingestion finished.
class HnswVectorIndex final : public VectorIndex {
 public:
  HnswVectorIndex();
  ~HnswVectorIndex() override;
  HnswVectorIndex(const HnswVectorIndex&) = delete;
  HnswVectorIndex& operator=(const HnswVectorIndex&) = delete;

  Status add(std::string_view chunk_id, std::vector<float> vector) override;

  std::vector<std::pair<std::string, float>> search(
      const std::vector<float>& query, std::size_t limit) const override;

  std::size_t size() const;
  std::size_t dimension() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Optional FAISS index. Built only when CF_HAS_FAISS is defined; returns a
// null pointer otherwise so callers can fall back to hnswlib.
std::unique_ptr<VectorIndex> make_faiss_vector_index();
std::unique_ptr<VectorIndex> make_faiss_ivfpq_vector_index();

}  // namespace cf::rag
