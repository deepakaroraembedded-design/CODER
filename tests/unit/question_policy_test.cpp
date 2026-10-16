// Unit tests for the RAG-backed question policy (design section 6.6.1 layer 3).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/orch/question_policy.hpp"

namespace {

using cf::orch::Question;
using cf::orch::QuestionAsked;
using cf::orch::QuestionDecision;
using cf::orch::QuestionOption;
using cf::orch::QuestionPolicyConfig;
using cf::orch::RagQuestionPolicy;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_question_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

QuestionAsked question_asked(const std::string& text,
                             const std::vector<std::string>& option_labels) {
  QuestionAsked asked;
  asked.session_id = "ses_1";
  asked.request_id = "que_1";
  Question question;
  question.text = text;
  for (const auto& label : option_labels) {
    question.options.push_back(QuestionOption{label, ""});
  }
  asked.questions.push_back(std::move(question));
  return asked;
}

class QuestionPolicyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    retriever_ = std::make_unique<cf::rag::InMemoryRetriever>();
    cf::rag::Document document;
    document.doc_id = "policy";
    document.version = "1";
    document.type = cf::rag::DocType::Policy;
    document.text =
        "# Retries\n\nNetwork retries use exponential backoff with jitter.\n";
    ASSERT_TRUE(retriever_->add_document(document).has_value());
  }

  void TearDown() override {
    retriever_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::rag::InMemoryRetriever> retriever_;
};

TEST_F(QuestionPolicyTest, AnswersWhenAnOptionMatchesADocument) {
  RagQuestionPolicy policy(*retriever_);
  const QuestionDecision decision = policy.decide(question_asked(
      "How should network retries behave?", {"Exponential backoff", "Fixed interval"}));
  EXPECT_TRUE(decision.answer);
  ASSERT_EQ(decision.answers.size(), 1u);
  ASSERT_EQ(decision.answers[0].size(), 1u);
  EXPECT_EQ(decision.answers[0][0], "Exponential backoff");
  EXPECT_EQ(policy.ambiguities_recorded(), 0u);
}

TEST_F(QuestionPolicyTest, RejectsAndRecordsWhenNothingCoversTheQuestion) {
  const auto ambiguities = dir_ / "ambiguities.jsonl";
  QuestionPolicyConfig config;
  config.ambiguities_file = ambiguities;
  RagQuestionPolicy policy(*retriever_, config);

  const QuestionDecision decision = policy.decide(
      question_asked("Which database shard key should I use?", {"User id", "Region"}));
  EXPECT_FALSE(decision.answer);
  EXPECT_NE(decision.reason.find("no high-precedence document"), std::string::npos);
  EXPECT_EQ(policy.ambiguities_recorded(), 1u);

  std::ifstream in(ambiguities);
  std::string line;
  ASSERT_TRUE(std::getline(in, line));
  const auto entry = nlohmann::json::parse(line);
  EXPECT_EQ(entry["question"], "Which database shard key should I use?");
  EXPECT_EQ(entry["request_id"], "que_1");
}

TEST_F(QuestionPolicyTest, RejectsWhenNoOptionMatchesThePassage) {
  RagQuestionPolicy policy(*retriever_);
  const QuestionDecision decision =
      policy.decide(question_asked("How should retries behave?", {"Postgres", "Kafka"}));
  EXPECT_FALSE(decision.answer);
  EXPECT_EQ(policy.ambiguities_recorded(), 1u);
}

TEST_F(QuestionPolicyTest, RejectsQuestionsWithoutOptions) {
  RagQuestionPolicy policy(*retriever_);
  const QuestionDecision decision =
      policy.decide(question_asked("How should retries behave?", {}));
  EXPECT_FALSE(decision.answer);
}

TEST_F(QuestionPolicyTest, HonoursTheMinimumScore) {
  QuestionPolicyConfig config;
  config.min_score = 0.5f;  // RRF scores are far below this
  RagQuestionPolicy policy(*retriever_, config);
  const QuestionDecision decision = policy.decide(
      question_asked("How should retries behave?", {"Exponential backoff"}));
  EXPECT_FALSE(decision.answer);
}

TEST_F(QuestionPolicyTest, RejectsTheWholeSetWhenOneQuestionIsUncovered) {
  QuestionAsked asked;
  asked.session_id = "ses_1";
  asked.request_id = "que_2";
  Question covered;
  covered.text = "How should retries behave?";
  covered.options.push_back(QuestionOption{"Exponential backoff", ""});
  Question uncovered;
  uncovered.text = "Which shard key should I use?";
  uncovered.options.push_back(QuestionOption{"User id", ""});
  asked.questions = {covered, uncovered};

  RagQuestionPolicy policy(*retriever_);
  const QuestionDecision decision = policy.decide(asked);
  EXPECT_FALSE(decision.answer);
  EXPECT_EQ(policy.ambiguities_recorded(), 1u);
}

TEST_F(QuestionPolicyTest, AnswersEveryQuestionWhenAllAreCovered) {
  QuestionAsked asked;
  asked.session_id = "ses_1";
  asked.request_id = "que_3";
  Question first;
  first.text = "How should retries behave?";
  first.options.push_back(QuestionOption{"Exponential backoff", ""});
  Question second;
  second.text = "What should retries use?";
  second.options.push_back(QuestionOption{"Exponential backoff", ""});
  asked.questions = {first, second};

  RagQuestionPolicy policy(*retriever_);
  const QuestionDecision decision = policy.decide(asked);
  EXPECT_TRUE(decision.answer);
  ASSERT_EQ(decision.answers.size(), 2u);
  EXPECT_EQ(decision.answers[1][0], "Exponential backoff");
}

}  // namespace
