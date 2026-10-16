// cf-rag: BM25 inverted index (design section 6.3, hybrid retrieval).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cf::rag {

class Bm25Index {
 public:
  explicit Bm25Index(double k1 = 1.2, double b = 0.75);

  void add(std::string_view chunk_id, std::string_view text);

  // Chunk ids ranked by BM25 score (descending), best first.
  std::vector<std::pair<std::string, double>> search(std::string_view query,
                                                     std::size_t limit) const;

  std::size_t size() const { return documents_.size(); }

 private:
  struct Document {
    std::size_t length = 0;
    std::unordered_map<std::string, std::size_t> term_freq;
  };

  double k1_;
  double b_;
  std::vector<std::pair<std::string, Document>> documents_;
  std::unordered_map<std::string, std::size_t> document_freq_;
  double average_length_ = 0.0;
};

}  // namespace cf::rag
