#include "cf/orch/question_policy.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace cf::orch {
namespace {

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

}  // namespace

RagQuestionPolicy::RagQuestionPolicy(rag::Retriever& retriever,
                                     QuestionPolicyConfig config)
    : retriever_(retriever), config_(std::move(config)) {}

void RagQuestionPolicy::record_ambiguity(const QuestionAsked& asked,
                                         const std::string& question,
                                         const std::string& reason) {
  ++ambiguities_recorded_;
  if (config_.ambiguities_file.empty()) {
    return;
  }
  std::ofstream out(config_.ambiguities_file, std::ios::app);
  if (!out) {
    return;
  }
  const nlohmann::json entry = {{"session_id", asked.session_id},
                                {"request_id", asked.request_id},
                                {"question", question},
                                {"reason", reason},
                                {"at", std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::system_clock::now()
                                               .time_since_epoch())
                                           .count()}};
  out << entry.dump() << "\n";
}

QuestionDecision RagQuestionPolicy::decide(const QuestionAsked& asked) {
  QuestionDecision decision;
  decision.answer = true;
  for (const auto& question : asked.questions) {
    rag::Query query;
    query.text = question.text;
    query.types = config_.answer_types;
    query.top_k = 1;
    auto hits = retriever_.search(query);

    std::string answer;
    if (hits.has_value() && !hits->empty() &&
        hits->front().score >= config_.min_score) {
      const std::string passage = to_lower(hits->front().text);
      for (const auto& option : question.options) {
        if (!option.label.empty() &&
            passage.find(to_lower(option.label)) != std::string::npos) {
          answer = option.label;
          break;
        }
      }
    }
    if (answer.empty()) {
      decision.answer = false;
      decision.reason =
          "no high-precedence document answers the question; recorded as an "
          "ambiguity";
      record_ambiguity(asked, question.text, decision.reason);
      return decision;
    }
    decision.answers.push_back({answer});
  }
  return decision;
}

}  // namespace cf::orch
