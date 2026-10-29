// cf-rag: optional FAISS vector index (design section 6.3).
#include "cf/rag/vector_index.hpp"

#include "cf/rag/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/index_io.h>

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

class FaissIvfPqVectorIndex final : public VectorIndex {
 public:
  explicit FaissIvfPqVectorIndex(FaissIvfPqOptions options)
      : options_(std::move(options)) {
    if (!options_.path.empty()) {
      load();
    }
  }

  Status add(std::string_view chunk_id, std::vector<float> vector) override {
    if (vector.empty()) {
      return fail(error_codes::kEmptyDocument, "empty embedding vector");
    }
    normalize(vector);
    const int dimension = static_cast<int>(vector.size());
    std::lock_guard<std::mutex> lock(mutex_);
    if (dimension_ == 0) {
      dimension_ = dimension;
    } else if (dimension != dimension_) {
      return fail(error_codes::kDimensionMismatch,
                  "embedding dimension mismatch");
    }
    labels_.emplace_back(chunk_id);
    if (index_ != nullptr) {
      index_->add(1, vector.data());
    } else {
      pending_.push_back(std::move(vector));
      if (pending_.size() >= options_.train_threshold) {
        train_and_flush();
      }
    }
    return {};
  }

  std::vector<std::pair<std::string, float>> search(
      const std::vector<float>& query, std::size_t limit) const override {
    std::vector<std::pair<std::string, float>> results;
    std::lock_guard<std::mutex> lock(mutex_);
    if (index_ == nullptr && !pending_.empty()) {
      const_cast<FaissIvfPqVectorIndex*>(this)->train_and_flush();
    }
    if (index_ == nullptr || limit == 0 ||
        query.size() != static_cast<std::size_t>(dimension_)) {
      return results;
    }
    std::vector<float> normalized = query;
    normalize(normalized);
    const std::size_t count = std::min(limit, labels_.size());
    std::vector<float> distances(count);
    std::vector<faiss::Index::idx_t> ids(count);
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
  void save() const {
    if (options_.path.empty() || index_ == nullptr) {
      return;
    }
    const std::string temporary = options_.path + ".tmp";
    faiss::write_index(index_.get(), temporary.c_str());
    std::ofstream labels(temporary + ".labels", std::ios::binary);
    for (const auto& label : labels_) {
      labels << label << "\n";
    }
    labels.close();
    std::error_code ec;
    std::filesystem::rename(temporary, options_.path, ec);
    std::filesystem::rename(temporary + ".labels",
                            options_.path + ".labels", ec);
  }

  void load() {
    std::error_code ec;
    if (!std::filesystem::exists(options_.path, ec)) {
      return;
    }
    std::unique_ptr<faiss::Index> loaded(
        faiss::read_index(options_.path.c_str()));
    auto* ivfpq = dynamic_cast<faiss::IndexIVFPQ*>(loaded.get());
    if (ivfpq == nullptr) {
      return;
    }
    loaded.release();
    index_.reset(ivfpq);
    dimension_ = index_->d;
    std::ifstream labels(options_.path + ".labels", std::ios::binary);
    std::string label;
    while (std::getline(labels, label)) {
      labels_.push_back(label);
    }
  }

  void train_and_flush() {
    if (pending_.empty() || dimension_ == 0) {
      return;
    }
    int m = std::min(8, dimension_);
    while (dimension_ % m != 0) {
      --m;
    }
    const bool small = pending_.size() < 10000;
    int nlist = small ? 1 : static_cast<int>(pending_.size()) / 39;
    if (options_.nlist > 0) {
      nlist = options_.nlist;
    }
    nlist = std::max(1, std::min(4096, nlist));
    const int nbits = small ? 1 : std::max(1, std::min(8, options_.nbits));
    auto quantizer = std::make_unique<faiss::IndexFlatIP>(dimension_);
    index_ = std::make_unique<faiss::IndexIVFPQ>(
        quantizer.get(), dimension_, nlist, m, nbits, faiss::METRIC_INNER_PRODUCT);
    index_->own_fields = false;
    index_->nprobe = std::max(1, std::min(nlist, options_.nprobe));
    quantizer_ = std::move(quantizer);
    std::vector<float> flat;
    flat.reserve(pending_.size() * dimension_);
    for (const auto& vector : pending_) {
      flat.insert(flat.end(), vector.begin(), vector.end());
    }
    index_->train(static_cast<faiss::Index::idx_t>(pending_.size()), flat.data());
    index_->add(static_cast<faiss::Index::idx_t>(pending_.size()), flat.data());
    pending_.clear();
    save();
  }

  FaissIvfPqOptions options_;
  mutable std::mutex mutex_;
  int dimension_ = 0;
  std::unique_ptr<faiss::IndexFlatIP> quantizer_;
  std::unique_ptr<faiss::IndexIVFPQ> index_;
  std::vector<std::vector<float>> pending_;
  std::vector<std::string> labels_;
};

}  // namespace

std::unique_ptr<VectorIndex> make_faiss_vector_index() {
  return std::make_unique<FaissVectorIndex>();
}

std::unique_ptr<VectorIndex> make_faiss_ivfpq_vector_index(
    FaissIvfPqOptions options) {
  return std::make_unique<FaissIvfPqVectorIndex>(options);
}

}  // namespace cf::rag
