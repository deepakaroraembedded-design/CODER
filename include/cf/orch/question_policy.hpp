// cf-orch: the RAG-backed question policy (design section 6.6.1 layer 3).
//
// A question is answered only when a high-precedence document covers it: the
// question is searched in the retrieval index, restricted to policy,
// requirements, architecture and design; an option is chosen when its label
// appears in the winning passage. Anything else is rejected and recorded as a
// requirement ambiguity for escalation; CF never makes up an answer.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "cf/orch/event_router.hpp"
#include "cf/rag/retriever.hpp"

namespace cf::orch {

struct QuestionPolicyConfig {
  // JSONL file for recorded ambiguities (empty: not recorded).
  std::filesystem::path ambiguities_file;
  // RAG applicability context (design section 6.3): the run's product and
  // target language, so scoped documents can cover a question.
  std::string product;
  std::string language;
  // Minimum retrieval score for a passage to count as coverage.
  float min_score = 0.0f;
  std::vector<rag::DocType> answer_types = {rag::DocType::Policy,
                                            rag::DocType::Requirements,
                                            rag::DocType::Architecture,
                                            rag::DocType::Design};
};

class RagQuestionPolicy {
 public:
  RagQuestionPolicy(rag::Retriever& retriever, QuestionPolicyConfig config = {});

  QuestionDecision decide(const QuestionAsked& asked);

  std::size_t ambiguities_recorded() const { return ambiguities_recorded_; }

 private:
  void record_ambiguity(const QuestionAsked& asked, const std::string& question,
                        const std::string& reason);

  rag::Retriever& retriever_;
  QuestionPolicyConfig config_;
  std::size_t ambiguities_recorded_ = 0;
};

}  // namespace cf::orch
