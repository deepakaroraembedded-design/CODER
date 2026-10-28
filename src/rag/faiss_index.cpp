// cf-rag: optional FAISS vector index (design section 6.3).
#include "cf/rag/vector_index.hpp"

#include "cf/rag/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <faiss/IndexHNSW.h>

namespace cf::rag {
namespace {

void normalize(std::vector<float>& vector) {
  double norm = 0.0;
  for (const float value : vector) {
    norm += static_cast<double>(value) * value;
  }
  if (norm == 0.0) {
    return;
  }
  const float scale = static_cast<float>(1.0 / std::sqrt(norm));
  for (float& value : vector) {
    value *= scale;
  }
}

class FaissVectorIndex final : public VectorIndex {
 public:
  FaissVectorIndex() = default;

  Status add(std::string_view chunk_id, std::vector<float> vector) override {
    if (vector.empty()) {
      return fail(error_codes::kEmptyDocument, "empty embedding vector");
    }
    normalize(vector);
    const int dimension = static_cast<int>(vector.size());
    std::lock_guard<std::mutex> lock(mutex_);
    if (dimension_ == 0) {
      dimension_ = dimension;
      index_ = std::make_unique<faiss::IndexHNSWFlat>(
          dimension_, 32, faiss::METRIC_INNER_PRODUCT);
      index_->hnsw.efConstruction = 80;
      index_->hnsw.efSearch = 64;
    } else if (dimension != dimension_) {
      return fail(error_codes::kDimensionMismatch,
                  "embedding dimension mismatch: expected " +
                      std::to_string(dimension_) + ", got " +
                      std::to_string(dimension));
    }
    index_->add(1, vector.data());
    labels_.emplace_back(chunk_id);
    return {};
  }

  std::vector<std::pair<std::string, float>> search(
      const std::vector<float>& query, std::size_t limit) const override {
    std::vector<std::pair<std::string, float>> results;
    if (limit == 0 || dimension_ == 0 || index_ == nullptr ||
        query.size() != static_cast<std::size_t>(dimension_)) {
      return results;
    }
    std::vector<float> normalized = query;
    normalize(normalized);
    const std::size_t count = std::min(limit, labels_.size());
    std::vector<float> distances(count);
    std::vector<faiss::Index::idx_t> ids(count);
    std::lock_guard<std::mutex> lock(mutex_);
    index_->search(1, normalized.data(), static_cast<faiss::Index::idx_t>(count),
                   distances.data(), ids.data());
    for (std::size_t i = 0; i < count; ++i) {
      if (ids[i] >= 0 && static_cast<std::size_t>(ids[i]) < labels_.size()) {
        results.emplace_back(labels_[static_cast<std::size_t>(ids[i])],
                             distances[i]);
      }
    }
    return results;
  }

 private:
  mutable std::mutex mutex_;
  int dimension_ = 0;
  std::unique_ptr<faiss::IndexHNSWFlat> index_;
  std::vector<std::string> labels_;
};

}  // namespace

std::unique_ptr<VectorIndex> make_faiss_vector_index() {
  return std::make_unique<FaissVectorIndex>();
}

}  // namespace cf::rag
