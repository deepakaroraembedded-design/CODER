// Unit tests for the RAG retrieval evaluation (design section 6.3): labeled
// sets, recall@k / MRR metrics, applicability context, and the cf-eval rag CLI.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/rag/evaluation.hpp"

namespace {

using cf::rag::Hit;
using cf::rag::Query;
using cf::rag::RetrievalEvalSet;
using cf::rag::Retriever;

Hit make_hit(const std::string& chunk_id, const std::string& doc_id) {
  Hit hit;
  hit.chunk_id = chunk_id;
  hit.doc_id = doc_id;
  return hit;
}

class FakeRetriever final : public Retriever {
 public:
  std::map<std::string, std::vector<Hit>> hits;
  mutable std::vector<Query> queries;  // recorded, to assert context propagation
  bool fail = false;

  cf::Result<std::vector<Hit>> search(const Query& query) const override {
    queries.push_back(query);
    if (fail) {
      return cf::fail("FAKE", "retriever unavailable");
    }
    const auto found = hits.find(query.text);
    return found == hits.end() ? std::vector<Hit>{} : found->second;
  }

  cf::Result<cf::rag::Document> get(std::string_view, std::string_view) const override {
    return cf::fail(cf::rag::error_codes::kNotFound, "not implemented");
  }
};

std::filesystem::path make_temp_dir() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_rag_eval_test_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

TEST(RagEvalSetTest, ParsesAndValidates) {
  auto set = cf::rag::parse_eval_set(R"yaml(
collection: company
k: 5
docs: docs
queries:
  - query: latency budget
    product: product-y
    relevant: [REQ@4.1#latency-budget]
  - query: naming rules
    relevant_docs: [CG]
    as_of: 1767225600
)yaml",
                                     "/tmp/sets");
  ASSERT_TRUE(set.has_value()) << set.error().message;
  EXPECT_EQ(set->collection, "company");
  EXPECT_EQ(set->k, 5u);
  EXPECT_EQ(set->docs_dir, std::filesystem::path("/tmp/sets/docs"));
  ASSERT_EQ(set->queries.size(), 2u);
  EXPECT_EQ(set->queries[0].context.product, "product-y");
  EXPECT_EQ(set->queries[1].context.as_of, 1767225600);

  const auto rejects = [](const std::string& yaml, const std::string& needle) {
    const auto parsed = cf::rag::parse_eval_set(yaml);
    EXPECT_FALSE(parsed.has_value()) << yaml;
    if (!parsed.has_value()) {
      EXPECT_NE(parsed.error().message.find(needle), std::string::npos)
          << parsed.error().message;
    }
  };
  rejects("k: 8\nqueries: []\n", "collection");
  rejects("collection: c\nk: 0\nqueries:\n  - query: q\n    relevant: [x]\n", "k must");
  rejects("collection: c\n", "queries");
  rejects("collection: c\nqueries:\n  - query: q\n", "no relevant");
  rejects("collection: c\nqueries:\n  - query: ''\n    relevant: [x]\n", "query needs text");
}

TEST(RagEvalTest, ComputesRecallAndMrr) {
  auto set = cf::rag::parse_eval_set(R"yaml(
collection: c
k: 8
queries:
  - query: q1
    relevant: [a]
  - query: q2
    relevant: [b, c]
  - query: q3
    relevant: [z]
)yaml");
  ASSERT_TRUE(set.has_value()) << set.error().message;

  FakeRetriever retriever;
  retriever.hits["q1"] = {make_hit("a", "D1"), make_hit("x", "D2")};
  retriever.hits["q2"] = {make_hit("x", "D2"), make_hit("b", "D1"), make_hit("c", "D1")};
  retriever.hits["q3"] = {make_hit("x", "D2"), make_hit("y", "D2")};

  auto metrics = cf::rag::evaluate_retrieval(retriever, *set);
  ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
  EXPECT_EQ(metrics->queries, 3u);
  EXPECT_EQ(metrics->misses, 1u);
  EXPECT_NEAR(metrics->recall, (1.0 + 1.0 + 0.0) / 3.0, 1e-9);
  EXPECT_NEAR(metrics->mrr, (1.0 + 0.5 + 0.0) / 3.0, 1e-9);
  EXPECT_FALSE(cf::rag::meets_retrieval_targets(*metrics));
}

TEST(RagEvalTest, CutoffLimitsTheRecall) {
  auto set = cf::rag::parse_eval_set(R"yaml(
collection: c
k: 2
queries:
  - query: q
    relevant: [b, c]
)yaml");
  ASSERT_TRUE(set.has_value()) << set.error().message;
  FakeRetriever retriever;
  retriever.hits["q"] = {make_hit("x", "D2"), make_hit("b", "D1"), make_hit("c", "D1")};
  auto metrics = cf::rag::evaluate_retrieval(retriever, *set);
  ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
  EXPECT_NEAR(metrics->recall, 0.5, 1e-9);  // c is beyond the cutoff
  EXPECT_NEAR(metrics->mrr, 0.5, 1e-9);
  ASSERT_EQ(retriever.queries.size(), 1u);
  EXPECT_EQ(retriever.queries[0].top_k, 2u);
}

TEST(RagEvalTest, DocumentLabelsMatchAnyChunk) {
  auto set = cf::rag::parse_eval_set(R"yaml(
collection: c
queries:
  - query: q
    relevant_docs: [CG]
)yaml");
  ASSERT_TRUE(set.has_value()) << set.error().message;
  FakeRetriever retriever;
  retriever.hits["q"] = {make_hit("CG@1#anywhere", "CG")};
  auto metrics = cf::rag::evaluate_retrieval(retriever, *set);
  ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
  EXPECT_DOUBLE_EQ(metrics->recall, 1.0);
  EXPECT_DOUBLE_EQ(metrics->mrr, 1.0);
}

TEST(RagEvalTest, CarriesTheApplicabilityContext) {
  auto set = cf::rag::parse_eval_set(R"yaml(
collection: c
queries:
  - query: q
    product: product-y
    component: message-bus
    language: cpp
    as_of: 1767225600
    relevant: [x]
)yaml");
  ASSERT_TRUE(set.has_value()) << set.error().message;
  FakeRetriever retriever;
  retriever.hits["q"] = {make_hit("x", "D")};
  auto metrics = cf::rag::evaluate_retrieval(retriever, *set);
  ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
  ASSERT_EQ(retriever.queries.size(), 1u);
  EXPECT_EQ(retriever.queries[0].context.product, "product-y");
  EXPECT_EQ(retriever.queries[0].context.component, "message-bus");
  EXPECT_EQ(retriever.queries[0].context.language, "cpp");
  EXPECT_EQ(retriever.queries[0].context.as_of, 1767225600);
}

TEST(RagEvalTest, PropagatesRetrieverErrors) {
  auto set = cf::rag::parse_eval_set("collection: c\nqueries:\n  - query: q\n    relevant: [x]\n");
  ASSERT_TRUE(set.has_value()) << set.error().message;
  FakeRetriever retriever;
  retriever.fail = true;
  EXPECT_FALSE(cf::rag::evaluate_retrieval(retriever, *set).has_value());
}

TEST(RagEvalTest, TargetThresholds) {
  cf::rag::RetrievalMetrics metrics;
  metrics.scored_queries = 10;
  metrics.recall = 0.85;
  metrics.mrr = 0.60;
  EXPECT_TRUE(cf::rag::meets_retrieval_targets(metrics));
  metrics.recall = 0.8499;
  EXPECT_FALSE(cf::rag::meets_retrieval_targets(metrics));
  metrics.recall = 0.9;
  metrics.mrr = 0.5999;
  EXPECT_FALSE(cf::rag::meets_retrieval_targets(metrics));

  // A conflict-only set has no relevance labels: the recall/MRR targets do
  // not apply, but every conflict expectation must be met.
  cf::rag::RetrievalMetrics conflicts;
  conflicts.conflict_queries = 2;
  conflicts.unresolved_conflict_queries = 1;
  EXPECT_TRUE(cf::rag::meets_retrieval_targets(conflicts));
  conflicts.conflict_misses = 1;
  EXPECT_FALSE(cf::rag::meets_retrieval_targets(conflicts));
  conflicts.conflict_misses = 0;
  conflicts.unresolved_conflict_misses = 1;
  EXPECT_FALSE(cf::rag::meets_retrieval_targets(conflicts));
}

TEST(RagEvalTest, ParsesAndEvaluatesConflictExpectations) {
  auto set = cf::rag::parse_eval_set(R"yaml(
collection: conflicts
queries:
  - query: status colour
    expect_unresolved_conflict: true
  - query: log length
    expect_conflict: true
)yaml");
  ASSERT_TRUE(set.has_value()) << set.error().message;
  ASSERT_EQ(set->queries.size(), 2u);
  EXPECT_TRUE(set->queries[0].expect_unresolved_conflict);
  EXPECT_TRUE(set->queries[1].expect_conflict);

  FakeRetriever retriever;
  Hit unresolved = make_hit("req-a@1#colour", "req-a");
  unresolved.conflict = true;
  unresolved.unresolved_conflict = true;
  unresolved.overridden_by = {"req-b@1#colour"};
  retriever.hits["status colour"] = {unresolved};
  Hit resolved = make_hit("policy@1#log", "policy");
  resolved.conflict = true;
  resolved.overridden_by = {"guide@1#log"};
  retriever.hits["log length"] = {resolved};

  auto metrics = cf::rag::evaluate_retrieval(retriever, *set);
  ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
  EXPECT_EQ(metrics->conflict_queries, 1u);
  EXPECT_EQ(metrics->conflict_misses, 0u);
  EXPECT_EQ(metrics->unresolved_conflict_queries, 1u);
  EXPECT_EQ(metrics->unresolved_conflict_misses, 0u);
  EXPECT_EQ(metrics->scored_queries, 0u);
  EXPECT_TRUE(cf::rag::meets_retrieval_targets(*metrics));

  // Missing the escalation is a miss.
  FakeRetriever plain;
  plain.hits["status colour"] = {make_hit("req-a@1#colour", "req-a")};
  plain.hits["log length"] = {make_hit("policy@1#log", "policy")};
  auto missed = cf::rag::evaluate_retrieval(plain, *set);
  ASSERT_TRUE(missed.has_value());
  EXPECT_EQ(missed->unresolved_conflict_misses, 1u);
  EXPECT_EQ(missed->conflict_misses, 1u);
  EXPECT_FALSE(cf::rag::meets_retrieval_targets(*missed));
}

TEST(RagEvalCliTest, EvaluatesTheStarterSet) {
  cf::process::LocalProcessExecutor executor;
  const std::string binary = std::string(CF_BINARY_DIR) + "/cf-eval";
  const std::string set = std::string(CF_SOURCE_DIR) + "/eval/rag/company.yaml";

  cf::process::ProcessRequest run;
  run.argv = {binary, "rag", "--set", set};
  run.timeout = std::chrono::seconds(30);
  const auto result = executor.run(run);
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_NE(result.stdout_text.find("targets met"), std::string::npos);
  EXPECT_NE(result.stdout_text.find("queries: 205"), std::string::npos);

  // The adversarial conflict set scores conflict expectations only.
  const std::string conflicts =
      std::string(CF_SOURCE_DIR) + "/eval/rag/conflicts.yaml";
  cf::process::ProcessRequest conflict_run;
  conflict_run.argv = {binary, "rag", "--set", conflicts};
  conflict_run.timeout = std::chrono::seconds(30);
  const auto conflict_result = executor.run(conflict_run);
  ASSERT_EQ(conflict_result.exit_code, 0) << conflict_result.stderr_text;
  EXPECT_NE(conflict_result.stdout_text.find("targets met"), std::string::npos);
  EXPECT_NE(conflict_result.stdout_text.find("unresolved-conflict queries: 2 (missed: 0)"),
            std::string::npos);

  cf::process::ProcessRequest chunks;
  chunks.argv = {binary, "rag", "--set", set, "--list-chunks"};
  chunks.timeout = std::chrono::seconds(30);
  const auto listed = executor.run(chunks);
  ASSERT_EQ(listed.exit_code, 0) << listed.stderr_text;
  EXPECT_NE(listed.stdout_text.find("REQ@4.1#latency-budget"), std::string::npos);

  // A set whose labels point at the wrong chunks misses the targets.
  const auto dir = make_temp_dir();
  write_file(dir / "bad.yaml", R"yaml(
collection: company
docs: )yaml" + std::string(CF_SOURCE_DIR) + R"yaml(/eval/rag/docs
queries:
  - query: p99 latency budget for publish subscribe delivery
    product: product-y
    relevant: [CG@3.2#logging-and-secrets]
)yaml");
  cf::process::ProcessRequest bad;
  bad.argv = {binary, "rag", "--set", (dir / "bad.yaml").string()};
  bad.timeout = std::chrono::seconds(30);
  const auto missed = executor.run(bad);
  EXPECT_EQ(missed.exit_code, 1) << missed.stderr_text;
  EXPECT_NE(missed.stdout_text.find("TARGETS MISSED"), std::string::npos);

  cf::process::ProcessRequest warned;
  warned.argv = {binary, "rag", "--set", (dir / "bad.yaml").string(), "--warn-only"};
  warned.timeout = std::chrono::seconds(30);
  EXPECT_EQ(executor.run(warned).exit_code, 0);

  cf::process::ProcessRequest missing;
  missing.argv = {binary, "rag", "--set", (dir / "missing.yaml").string()};
  missing.timeout = std::chrono::seconds(30);
  EXPECT_EQ(executor.run(missing).exit_code, 2);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
