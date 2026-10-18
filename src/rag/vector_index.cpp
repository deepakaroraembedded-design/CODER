#include "cf/rag/vector_index.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

#include <hnswlib/hnswlib.h>

#include "cf/rag/types.hpp"

namespace cf::rag {
namespace {

std::vector<float> normalize(const std::vector<float>& vector) {
  double norm = 0.0;
  for (const float value : vector) {
    norm += static_cast<double>(value) * static_cast<double>(value);
  }
  norm = std::sqrt(norm);
  if (norm == 0.0) {
    return vector;
  }
  std::vector<float> out(vector.size());
  for (std::size_t i = 0; i < vector.size(); ++i) {
    out[i] = static_cast<float>(static_cast<double>(vector[i]) / norm);
  }
  return out;
}

}  // namespace

Status BruteForceVectorIndex::add(std::string_view chunk_id,
                                  std::vector<float> vector) {
  if (dimension_ == 0) {
    dimension_ = vector.size();
  } else if (vector.size() != dimension_) {
    return fail(error_codes::kDimensionMismatch,
                "expected dimension " + std::to_string(dimension_) + ", got " +
                    std::to_string(vector.size()));
  }
  entries_.emplace_back(std::string(chunk_id), normalize(vector));
  return {};
}

std::vector<std::pair<std::string, float>> BruteForceVectorIndex::search(
    const std::vector<float>& query, std::size_t limit) const {
  std::vector<std::pair<std::string, float>> results;
  if (entries_.empty() || limit == 0 || query.size() != dimension_) {
    return results;
  }
  const std::vector<float> normalized = normalize(query);
  std::vector<std::pair<std::string, float>> scored;
  scored.reserve(entries_.size());
  for (const auto& [chunk_id, vector] : entries_) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < vector.size(); ++i) {
      dot += vector[i] * normalized[i];
    }
    scored.emplace_back(chunk_id, dot);
  }
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second;
    }
    return a.first < b.first;  // deterministic tie-break
  });
  if (scored.size() > limit) {
    scored.resize(limit);
  }
  return scored;
}

namespace {

// HNSW parameters (design section 6.3): M and ef_construction trade build time
// for recall; ef_search trades query time for recall.
constexpr std::size_t kM = 16;
constexpr std::size_t kEfConstruction = 200;
constexpr std::size_t kEfSearch = 64;
constexpr std::size_t kInitialCapacity = 1024;

}  // namespace

struct HnswVectorIndex::Impl {
  std::size_t dimension = 0;
  std::unique_ptr<hnswlib::InnerProductSpace> space;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> index;
  std::vector<std::string> labels;  // label -> chunk_id
};

HnswVectorIndex::HnswVectorIndex() : impl_(std::make_unique<Impl>()) {}

HnswVectorIndex::~HnswVectorIndex() = default;

Status HnswVectorIndex::add(std::string_view chunk_id, std::vector<float> vector) {
  if (vector.empty()) {
    return fail(error_codes::kDimensionMismatch, "cannot index an empty vector");
  }
  if (impl_->dimension == 0) {
    impl_->dimension = vector.size();
    impl_->space = std::make_unique<hnswlib::InnerProductSpace>(impl_->dimension);
    impl_->index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        impl_->space.get(), kInitialCapacity, kM, kEfConstruction);
    impl_->index->setEf(kEfSearch);
  } else if (vector.size() != impl_->dimension) {
    return fail(error_codes::kDimensionMismatch,
                "expected dimension " + std::to_string(impl_->dimension) + ", got " +
                    std::to_string(vector.size()));
  }
  if (impl_->labels.size() >= impl_->index->max_elements_) {
    impl_->index->resizeIndex(impl_->index->max_elements_ * 2);
  }
  const std::vector<float> normalized = normalize(vector);
  const hnswlib::labeltype label =
      static_cast<hnswlib::labeltype>(impl_->labels.size());
  impl_->labels.emplace_back(chunk_id);
  impl_->index->addPoint(normalized.data(), label);
  return {};
}

std::vector<std::pair<std::string, float>> HnswVectorIndex::search(
    const std::vector<float>& query, std::size_t limit) const {
  std::vector<std::pair<std::string, float>> results;
  if (impl_->index == nullptr || limit == 0 ||
      query.size() != impl_->dimension || impl_->labels.empty()) {
    return results;
  }
  const std::vector<float> normalized = normalize(query);
  const std::size_t k = std::min(limit, impl_->labels.size());
  std::priority_queue<std::pair<float, hnswlib::labeltype>> nearest =
      impl_->index->searchKnn(normalized.data(), k);
  std::vector<std::pair<float, hnswlib::labeltype>> ordered;
  ordered.reserve(nearest.size());
  while (!nearest.empty()) {
    ordered.push_back(nearest.top());
    nearest.pop();
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) {
      return a.first < b.first;
    }
    return a.second < b.second;  // deterministic tie-break
  });
  results.reserve(ordered.size());
  for (const auto& [distance, label] : ordered) {
    // InnerProductSpace reports 1 - dot; the vectors are normalized, so the
    // similarity is the cosine.
    results.emplace_back(impl_->labels[label], 1.0f - distance);
  }
  return results;
}

std::size_t HnswVectorIndex::size() const { return impl_->labels.size(); }

std::size_t HnswVectorIndex::dimension() const { return impl_->dimension; }

}  // namespace cf::rag
