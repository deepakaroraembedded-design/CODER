#include "cf/rag/bm25.hpp"

#include <algorithm>
#include <cmath>

#include "cf/rag/chunker.hpp"

namespace cf::rag {

Bm25Index::Bm25Index(double k1, double b) : k1_(k1), b_(b) {}

void Bm25Index::add(std::string_view chunk_id, std::string_view text) {
  const std::vector<std::string> tokens = tokenize(text);
  Document document;
  document.length = tokens.size();
  for (const auto& token : tokens) {
    ++document.term_freq[token];
  }
  for (const auto& [term, count] : document.term_freq) {
    (void)count;
    ++document_freq_[term];
  }
  const double total =
      average_length_ * static_cast<double>(documents_.size()) +
      static_cast<double>(document.length);
  documents_.emplace_back(std::string(chunk_id), std::move(document));
  average_length_ = total / static_cast<double>(documents_.size());
}

std::vector<std::pair<std::string, double>> Bm25Index::search(std::string_view query,
                                                              std::size_t limit) const {
  std::vector<std::pair<std::string, double>> results;
  if (documents_.empty() || limit == 0) {
    return results;
  }
  const std::vector<std::string> query_tokens = tokenize(query);
  const double n = static_cast<double>(documents_.size());

  std::vector<std::pair<std::string, double>> scored;
  for (const auto& [chunk_id, document] : documents_) {
    double score = 0.0;
    for (const auto& token : query_tokens) {
      const auto term = document.term_freq.find(token);
      if (term == document.term_freq.end()) {
        continue;
      }
      const auto df = document_freq_.find(token);
      const double document_frequency = df == document_freq_.end() ? 1.0 : static_cast<double>(df->second);
      const double idf =
          std::log(1.0 + (n - document_frequency + 0.5) / (document_frequency + 0.5));
      const double tf = static_cast<double>(term->second);
      const double norm =
          1.0 - b_ + b_ * static_cast<double>(document.length) /
                         (average_length_ > 0.0 ? average_length_ : 1.0);
      score += idf * (tf * (k1_ + 1.0)) / (tf + k1_ * norm);
    }
    if (score > 0.0) {
      scored.emplace_back(chunk_id, score);
    }
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) {
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
