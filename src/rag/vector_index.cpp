#include "cf/rag/vector_index.hpp"

#include <algorithm>
#include <cmath>

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

}  // namespace cf::rag
