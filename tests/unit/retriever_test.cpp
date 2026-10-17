// Unit tests for the hybrid retriever: applicability filter, precedence and
// conflict resolution (design section 6.3).
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cf/rag/retriever.hpp"

namespace {

using cf::rag::Chunk;
using cf::rag::Document;
using cf::rag::DocType;
using cf::rag::Hit;
using cf::rag::InMemoryRetriever;
using cf::rag::Query;

// Deterministic bag-of-words embedding: cosine similarity approximates token
// overlap, which is enough to exercise the vector half of hybrid retrieval.
class HashingEmbedder final : public cf::rag::Embedder {
 public:
  explicit HashingEmbedder(std::size_t dimension = 64) : dimension_(dimension) {}

  cf::Result<std::vector<std::vector<float>>> embed(
      const std::vector<std::string>& texts) override {
    std::vector<std::vector<float>> out;
    out.reserve(texts.size());
    for (const auto& text : texts) {
      std::vector<float> vector(dimension_, 0.0f);
      for (const auto& token : cf::rag::tokenize(text)) {
        vector[std::hash<std::string>{}(token) % dimension_] += 1.0f;
      }
      out.push_back(std::move(vector));
    }
    return out;
  }

 private:
  std::size_t dimension_;
};

Document make_document(const std::string& doc_id, DocType type,
                       const std::string& text) {
  Document document;
  document.doc_id = doc_id;
  document.version = "1";
  document.type = type;
  document.text = text;
  document.source = "repo://docs/" + doc_id + ".md";
  return document;
}

Chunk make_chunk(const std::string& doc_id, DocType type, const std::string& text,
                 const std::string& topic) {
  Chunk chunk;
  chunk.chunk_id = doc_id + "@1#" + topic;
  chunk.doc_id = doc_id;
  chunk.version = "1";
  chunk.text = text;
  chunk.citation = "repo://docs/" + doc_id + ".md@1#" + topic;
  chunk.type = type;
  chunk.topic = topic;
  chunk.status = "active";
  chunk.scope = "company";
  return chunk;
}

TEST(RetrieverTest, SearchReturnsTheRelevantDocument) {
  InMemoryRetriever retriever;
  ASSERT_TRUE(retriever
                  .add_document(make_document(
                      "policy", DocType::Policy,
                      "# Security\n\nAll secrets must be stored in the secret manager and "
                      "never logged.\n"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_document(make_document("guide", DocType::Guideline,
                                              "# Formatting\n\nUse clang-format for all "
                                              "C++ sources.\n"))
                  .has_value());

  Query query;
  query.text = "secret manager logging";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value()) << hits.error().message;
  ASSERT_FALSE(hits->empty());
  EXPECT_EQ((*hits)[0].doc_id, "policy");
  EXPECT_NE((*hits)[0].citation.find("policy"), std::string::npos);
  EXPECT_FALSE((*hits)[0].conflict);
}

TEST(RetrieverTest, HybridSearchWithEmbedder) {
  InMemoryRetriever retriever;
  retriever.set_embedder(std::make_shared<HashingEmbedder>());
  ASSERT_TRUE(retriever
                  .add_document(make_document("policy", DocType::Policy,
                                              "# Security\n\nRotate credentials every 90 "
                                              "days in the secret manager.\n"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_document(make_document("guide", DocType::Guideline,
                                              "# Formatting\n\nUse clang-format for all "
                                              "C++ sources.\n"))
                  .has_value());

  Query query;
  query.text = "clang-format C++ sources";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value()) << hits.error().message;
  ASSERT_FALSE(hits->empty());
  EXPECT_EQ((*hits)[0].doc_id, "guide");
}

TEST(RetrieverTest, InactiveStatusesAreFiltered) {
  InMemoryRetriever retriever;
  Document superseded = make_document("old", DocType::Guideline,
                                      "# Old\n\nZebra protocol is superseded.\n");
  superseded.status = "superseded";
  ASSERT_TRUE(retriever.add_document(superseded).has_value());
  Document draft = make_document("draft", DocType::Guideline,
                                 "# Draft\n\nZebra protocol draft text.\n");
  draft.status = "draft";
  ASSERT_TRUE(retriever.add_document(draft).has_value());

  Query query;
  query.text = "zebra protocol";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());
}

TEST(RetrieverTest, ApprovedDesignDocumentsAreRetrievable) {
  InMemoryRetriever retriever;
  // Approved design packages (design section 6.2 step 7) are retrievable for
  // their product; once superseded they are not.
  Document approved = make_document("design-PLAN-A-R1", DocType::Design,
                                    "# Design\n\nZebra protocol design decision.\n");
  approved.status = "approved";
  approved.scope = "product";
  approved.scope_value = "zebra";
  ASSERT_TRUE(retriever.add_document(approved).has_value());

  Query query;
  query.text = "zebra protocol design";
  query.context.product = "zebra";
  auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value()) << hits.error().message;
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ(hits->front().doc_id, "design-PLAN-A-R1");

  // Another product does not see it.
  query.context.product = "other";
  hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());

  // A superseded design is not returned even for its own product.
  InMemoryRetriever superseded_retriever;
  Document superseded = approved;
  superseded.status = "superseded";
  ASSERT_TRUE(superseded_retriever.add_document(superseded).has_value());
  query.context.product = "zebra";
  hits = superseded_retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());
}

TEST(RetrieverTest, ScopeMustMatchTheRunContext) {
  InMemoryRetriever retriever;
  Document component_doc = make_document(
      "registry-rules", DocType::Guideline, "# Registry\n\nRegistryThreadSafety rules.\n");
  component_doc.scope = "component";
  component_doc.scope_value = "registry";
  ASSERT_TRUE(retriever.add_document(component_doc).has_value());

  Query query;
  query.text = "RegistryThreadSafety";
  query.context.component = "registry";
  auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);

  query.context.component = "bus";
  hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());

  query.context.component.clear();
  hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());
}

TEST(RetrieverTest, EffectiveDatesAreApplied) {
  InMemoryRetriever retriever;
  Document document = make_document("timed", DocType::Policy,
                                    "# Timed\n\nVolcano retention policy.\n");
  document.effective_from = 100;
  document.effective_to = 200;
  ASSERT_TRUE(retriever.add_document(document).has_value());

  Query query;
  query.text = "volcano retention";
  query.context.as_of = 50;
  auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());

  query.context.as_of = 150;
  hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);

  query.context.as_of = 250;
  hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());
}

TEST(RetrieverTest, PrecedenceResolvesTopicConflict) {
  InMemoryRetriever retriever;
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("policy", DocType::Policy,
                                        "Line coverage must be at least 85 percent.", "CG-5.1"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("guide", DocType::Guideline,
                                        "Line coverage should be at least 70 percent.",
                                        "CG-5.1"))
                  .has_value());

  Query query;
  query.text = "line coverage percent";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value()) << hits.error().message;
  ASSERT_EQ(hits->size(), 1u);
  const Hit& hit = (*hits)[0];
  EXPECT_EQ(hit.type, DocType::Policy);
  EXPECT_TRUE(hit.conflict);
  EXPECT_FALSE(hit.unresolved_conflict);
  ASSERT_EQ(hit.overridden_by.size(), 1u);
  EXPECT_EQ(hit.overridden_by[0], "guide@1#CG-5.1");
  EXPECT_NE(hit.deciding_rule.find("precedence"), std::string::npos);
}

TEST(RetrieverTest, RequirementConflictsAreUnresolved) {
  InMemoryRetriever retriever;
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("srs-a", DocType::Requirements,
                                        "The bus latency must be under 2 ms.", "REQ-001"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("srs-b", DocType::Requirements,
                                        "The bus latency must be under 5 ms.", "REQ-001"))
                  .has_value());

  Query query;
  query.text = "bus latency";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_TRUE((*hits)[0].conflict);
  EXPECT_TRUE((*hits)[0].unresolved_conflict);
  EXPECT_NE((*hits)[0].deciding_rule.find("requirement conflict"), std::string::npos);
}

TEST(RetrieverTest, RequirementAgainstPolicyIsUnresolved) {
  InMemoryRetriever retriever;
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("policy", DocType::Policy,
                                        "Audit logs must be kept for 1 year.", "RET-9"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("srs", DocType::Requirements,
                                        "Audit logs must be kept for 30 days.", "RET-9"))
                  .has_value());

  Query query;
  query.text = "audit logs kept";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ((*hits)[0].type, DocType::Policy);
  EXPECT_TRUE((*hits)[0].unresolved_conflict);
}

TEST(RetrieverTest, ScopeSpecificityWinsWithinAType) {
  InMemoryRetriever retriever;
  Chunk company = make_chunk("company", DocType::Guideline,
                             "Retries use exponential backoff.", "RETRY");
  ASSERT_TRUE(retriever.add_chunk(company).has_value());
  Chunk component = make_chunk("component", DocType::Guideline,
                               "Retries use fixed intervals in the registry.", "RETRY");
  component.scope = "component";
  component.scope_value = "registry";
  ASSERT_TRUE(retriever.add_chunk(component).has_value());

  Query query;
  query.text = "retries backoff intervals";
  query.context.component = "registry";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ((*hits)[0].doc_id, "component");
  EXPECT_NE((*hits)[0].deciding_rule.find("scope"), std::string::npos);
}

TEST(RetrieverTest, LaterEffectiveDateWinsWithinAType) {
  InMemoryRetriever retriever;
  Chunk older = make_chunk("older", DocType::Guideline, "Threads use a global lock.", "LOCK");
  older.effective_from = 100;
  ASSERT_TRUE(retriever.add_chunk(older).has_value());
  Chunk newer = make_chunk("newer", DocType::Guideline,
                           "Threads use a fine-grained lock.", "LOCK");
  newer.effective_from = 200;
  ASSERT_TRUE(retriever.add_chunk(newer).has_value());

  Query query;
  query.text = "threads lock";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ((*hits)[0].doc_id, "newer");
  EXPECT_NE((*hits)[0].deciding_rule.find("effective date"), std::string::npos);
}

TEST(RetrieverTest, SameTypeScopeAndDateIsUnresolved) {
  InMemoryRetriever retriever;
  Chunk first = make_chunk("first", DocType::Guideline, "Coverage target is 80 percent.",
                           "COV");
  first.effective_from = 100;
  ASSERT_TRUE(retriever.add_chunk(first).has_value());
  Chunk second = make_chunk("second", DocType::Guideline, "Coverage target is 90 percent.",
                            "COV");
  second.effective_from = 100;
  ASSERT_TRUE(retriever.add_chunk(second).has_value());

  Query query;
  query.text = "coverage target percent";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_TRUE((*hits)[0].conflict);
  EXPECT_TRUE((*hits)[0].unresolved_conflict);
  EXPECT_NE((*hits)[0].deciding_rule.find("same type, scope and effective date"),
            std::string::npos);
}

TEST(RetrieverTest, TopKLimitIsRespected) {
  InMemoryRetriever retriever;
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(retriever
                    .add_document(make_document(
                        "doc" + std::to_string(i), DocType::Guideline,
                        "# Doc\n\nStreaming compression pipeline details.\n"))
                    .has_value());
  }
  Query query;
  query.text = "streaming compression pipeline";
  query.top_k = 2;
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  EXPECT_EQ(hits->size(), 2u);
}

TEST(RetrieverTest, TypeFilterIsApplied) {
  InMemoryRetriever retriever;
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("policy", DocType::Policy,
                                        "Deployment windows are Tuesdays.", "DEP"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_chunk(make_chunk("guide", DocType::Guideline,
                                        "Deployment windows use a canary.", "DEP"))
                  .has_value());

  Query query;
  query.text = "deployment windows";
  query.types = {DocType::Policy};
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ((*hits)[0].type, DocType::Policy);
}

TEST(RetrieverTest, UnrelatedDocumentsAreNotConflicted) {
  InMemoryRetriever retriever;
  ASSERT_TRUE(retriever
                  .add_document(make_document("a", DocType::Guideline,
                                              "# A\n\nKafka topics use compaction.\n"))
                  .has_value());
  ASSERT_TRUE(retriever
                  .add_document(make_document("b", DocType::Guideline,
                                              "# B\n\nKafka consumers use manual commits.\n"))
                  .has_value());

  Query query;
  query.text = "kafka topics consumers commits";
  const auto hits = retriever.search(query);
  ASSERT_TRUE(hits.has_value());
  ASSERT_GE(hits->size(), 1u);
  for (const auto& hit : *hits) {
    EXPECT_FALSE(hit.conflict);
  }
}

TEST(RetrieverTest, GetByVersionAndLatest) {
  InMemoryRetriever retriever;
  Document v1 = make_document("spec", DocType::Architecture, "# Spec\n\nVersion one.\n");
  v1.version = "1";
  v1.effective_from = 100;
  ASSERT_TRUE(retriever.add_document(v1).has_value());
  Document v2 = make_document("spec", DocType::Architecture, "# Spec\n\nVersion two.\n");
  v2.version = "2";
  v2.effective_from = 200;
  ASSERT_TRUE(retriever.add_document(v2).has_value());

  const auto latest = retriever.get("spec", "");
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->version, "2");

  const auto exact = retriever.get("spec", "1");
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(exact->version, "1");

  const auto missing = retriever.get("nope", "");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, cf::rag::error_codes::kNotFound);
}

TEST(RetrieverTest, DuplicateDocumentIsRejected) {
  InMemoryRetriever retriever;
  const Document document =
      make_document("dup", DocType::Policy, "# Dup\n\nUnique content here.\n");
  ASSERT_TRUE(retriever.add_document(document).has_value());
  const auto again = retriever.add_document(document);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, cf::rag::error_codes::kDuplicateDocument);
}

}  // namespace
