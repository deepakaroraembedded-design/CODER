// Unit tests for RAG types, tokenizer, chunker, BM25 and the vector index
// (design section 6.3).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cf/rag/bm25.hpp"
#include "cf/rag/chunker.hpp"
#include "cf/rag/types.hpp"
#include "cf/rag/vector_index.hpp"

namespace {

std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  for (const char c : text) {
    if (c == ' ' || c == '\n') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

TEST(RagTypesTest, DocTypeRoundTrip) {
  EXPECT_EQ(cf::rag::to_string(cf::rag::DocType::Policy), "policy");
  EXPECT_EQ(cf::rag::doc_type_from_string("external_reference"),
            cf::rag::DocType::ExternalReference);
  EXPECT_EQ(cf::rag::doc_type_from_string("nonsense"), std::nullopt);
}

TEST(RagTypesTest, PrecedenceOrder) {
  EXPECT_LT(cf::rag::precedence_rank(cf::rag::DocType::Policy),
            cf::rag::precedence_rank(cf::rag::DocType::Requirements));
  EXPECT_LT(cf::rag::precedence_rank(cf::rag::DocType::Requirements),
            cf::rag::precedence_rank(cf::rag::DocType::Architecture));
  EXPECT_LT(cf::rag::precedence_rank(cf::rag::DocType::Design),
            cf::rag::precedence_rank(cf::rag::DocType::Guideline));
  EXPECT_LT(cf::rag::precedence_rank(cf::rag::DocType::Pattern),
            cf::rag::precedence_rank(cf::rag::DocType::ExternalReference));
}

TEST(RagTypesTest, ScopeSpecificity) {
  EXPECT_GT(cf::rag::scope_specificity("component"),
            cf::rag::scope_specificity("product"));
  EXPECT_GT(cf::rag::scope_specificity("product"),
            cf::rag::scope_specificity("company"));
}

TEST(RagTokenizerTest, LowercasesAndSplits) {
  const auto tokens = cf::rag::tokenize("Hello, World! foo_bar-baz 42");
  const std::vector<std::string> expected = {"hello", "world", "foo_bar-baz", "42"};
  EXPECT_EQ(tokens, expected);
}

TEST(RagTokenizerTest, EmptyInput) {
  EXPECT_TRUE(cf::rag::tokenize("").empty());
  EXPECT_TRUE(cf::rag::tokenize("... --- ...").empty());
}

TEST(RagTokenizerTest, EstimateTokens) {
  EXPECT_EQ(cf::rag::estimate_tokens("one two three"), 3u);
  EXPECT_EQ(cf::rag::estimate_tokens("  spaced\n\nwords "), 2u);
  EXPECT_EQ(cf::rag::estimate_tokens(""), 0u);
}

TEST(RagChunkerTest, MarkdownSplitsByHeading) {
  const std::string text =
      "Intro line.\n\n# First Section\n\nBody one.\n\n## Second Section\n\nBody two.\n";
  const auto chunks = cf::rag::chunk_markdown(text);
  ASSERT_EQ(chunks.size(), 3u);
  EXPECT_EQ(chunks[0].anchor, "intro");
  EXPECT_EQ(chunks[1].anchor, "first-section");
  EXPECT_EQ(chunks[2].anchor, "second-section");
  EXPECT_NE(chunks[1].text.find("Body one"), std::string::npos);
  EXPECT_NE(chunks[2].text.find("Body two"), std::string::npos);
}

TEST(RagChunkerTest, LongSectionSplitsWithOverlap) {
  std::string text = "# Long\n\n";
  for (int i = 0; i < 100; ++i) {
    text += "word" + std::to_string(i) + " ";
  }
  cf::rag::ChunkOptions options;
  options.max_tokens = 20;
  options.overlap = 0.25;
  const auto chunks = cf::rag::chunk_markdown(text, options);
  ASSERT_GE(chunks.size(), 2u);
  EXPECT_EQ(chunks[0].anchor, "long-part-1");
  EXPECT_EQ(chunks[1].anchor, "long-part-2");

  const auto first = words(chunks[0].text);
  const auto second = words(chunks[1].text);
  ASSERT_GE(first.size(), 5u);
  ASSERT_GE(second.size(), 5u);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(first[first.size() - 5 + i], second[i]);
  }
}

TEST(RagChunkerTest, PlainTextRespectsMinAndMax) {
  std::string text;
  for (int i = 0; i < 6; ++i) {
    text += "paragraph" + std::to_string(i) + " one two three four five six seven eight "
                                               "nine ten\n\n";
  }
  cf::rag::ChunkOptions options;
  options.min_tokens = 22;
  options.max_tokens = 40;
  options.overlap = 0.0;
  const auto chunks = cf::rag::chunk_text(text, options);
  ASSERT_EQ(chunks.size(), 3u);
  for (const auto& chunk : chunks) {
    EXPECT_LE(cf::rag::estimate_tokens(chunk.text), options.max_tokens);
  }
}

TEST(RagBm25Test, RanksRelevantChunksFirst) {
  cf::rag::Bm25Index index;
  index.add("a", "the quick brown fox jumps");
  index.add("b", "lazy dog sleeps all day");
  index.add("c", "quick quick fox");
  const auto results = index.search("quick fox", 3);
  ASSERT_EQ(results.size(), 2u);  // doc "b" shares no query term
  EXPECT_EQ(results[0].first, "c");
  EXPECT_EQ(results[1].first, "a");
}

TEST(RagBm25Test, NoMatchReturnsNothing) {
  cf::rag::Bm25Index index;
  index.add("a", "the quick brown fox");
  EXPECT_TRUE(index.search("zebra", 5).empty());
}

TEST(RagVectorIndexTest, RanksByCosineSimilarity) {
  cf::rag::BruteForceVectorIndex index;
  ASSERT_TRUE(index.add("x", {1.0f, 0.0f}).has_value());
  ASSERT_TRUE(index.add("y", {0.0f, 1.0f}).has_value());
  const auto results = index.search({0.9f, 0.1f}, 2);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].first, "x");
  EXPECT_GT(results[0].second, results[1].second);
}

TEST(RagVectorIndexTest, RejectsDimensionMismatch) {
  cf::rag::BruteForceVectorIndex index;
  ASSERT_TRUE(index.add("x", {1.0f, 0.0f}).has_value());
  const auto mismatch = index.add("z", {1.0f, 2.0f, 3.0f});
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(mismatch.error().code, cf::rag::error_codes::kDimensionMismatch);
}

TEST(RagVectorIndexTest, EmptyIndexReturnsNothing) {
  cf::rag::BruteForceVectorIndex index;
  EXPECT_TRUE(index.search({1.0f}, 5).empty());
}

TEST(RagVectorIndexTest, HnswRanksByCosineSimilarity) {
  cf::rag::HnswVectorIndex index;
  ASSERT_TRUE(index.add("x", {1.0f, 0.0f}).has_value());
  ASSERT_TRUE(index.add("y", {0.0f, 1.0f}).has_value());
  ASSERT_TRUE(index.add("z", {0.7f, 0.7f}).has_value());
  EXPECT_EQ(index.size(), 3u);
  EXPECT_EQ(index.dimension(), 2u);
  const auto results = index.search({0.9f, 0.1f}, 3);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].first, "x");
  EXPECT_GT(results[0].second, results[1].second);
  EXPECT_GT(results[1].second, results[2].second);
}

TEST(RagVectorIndexTest, HnswRejectsDimensionMismatch) {
  cf::rag::HnswVectorIndex index;
  ASSERT_TRUE(index.add("x", {1.0f, 0.0f}).has_value());
  const auto mismatch = index.add("z", {1.0f, 2.0f, 3.0f});
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(mismatch.error().code, cf::rag::error_codes::kDimensionMismatch);
  EXPECT_TRUE(index.search({1.0f, 2.0f, 3.0f}, 1).empty());
}

// Sanitizer detection for the skip below (GCC and Clang spell it
// differently).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CF_TEST_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CF_TEST_SANITIZER 1
#endif
#endif

TEST(RagVectorIndexTest, HnswMatchesBruteForceAndGrows) {
#ifdef CF_TEST_SANITIZER
  // hnswlib's SSE prefetch reads one pointer past a neighbour list
  // (nmslib/hnswlib#431) and its per-element locks invert order under TSan;
  // both are benign here but sanitizers flag them, and GCC 11's runtimes
  // cannot suppress them (no `fun:`/`deadlock:` support). The small HNSW
  // tests above still exercise the wrapper under every sanitizer.
  GTEST_SKIP() << "hnswlib internals trip ASan/TSan";
#endif
  // Deterministic pseudo-random vectors; the approximate index must agree
  // with the exact one on the nearest neighbour and grow past its initial
  // capacity.
  cf::rag::BruteForceVectorIndex exact;
  cf::rag::HnswVectorIndex approximate;
  std::uint64_t state = 42;
  const auto next = [&state]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<float>((state >> 33) % 1000) / 1000.0f - 0.5f;
  };
  constexpr std::size_t kCount = 1500;
  for (std::size_t i = 0; i < kCount; ++i) {
    std::vector<float> vector(16);
    for (auto& value : vector) {
      value = next();
    }
    const std::string id = "chunk-" + std::to_string(i);
    ASSERT_TRUE(exact.add(id, vector).has_value());
    ASSERT_TRUE(approximate.add(id, vector).has_value());
  }
  EXPECT_EQ(approximate.size(), kCount);  // grew past the initial capacity

  for (int query_index = 0; query_index < 10; ++query_index) {
    std::vector<float> query(16);
    for (auto& value : query) {
      value = next();
    }
    const auto exact_top = exact.search(query, 1);
    const auto approximate_top = approximate.search(query, 1);
    ASSERT_EQ(exact_top.size(), 1u);
    ASSERT_EQ(approximate_top.size(), 1u);
    EXPECT_EQ(exact_top[0].first, approximate_top[0].first);
    EXPECT_NEAR(exact_top[0].second, approximate_top[0].second, 0.001f);
  }
}

}  // namespace

#ifdef CF_HAS_FAISS
TEST(RagVectorIndexTest, FaissRanksByCosineSimilarity) {
  auto index = cf::rag::make_faiss_vector_index();
  ASSERT_NE(index, nullptr);
  ASSERT_TRUE(index->add("x", {1.0f, 0.0f}).has_value());
  ASSERT_TRUE(index->add("y", {0.0f, 1.0f}).has_value());
  ASSERT_TRUE(index->add("z", {0.7f, 0.7f}).has_value());
  const auto results = index->search({0.9f, 0.1f}, 3);
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].first, "x");
  EXPECT_GT(results[0].second, results[1].second);

  const auto mismatch = index->add("bad", {1.0f, 2.0f, 3.0f});
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(mismatch.error().code, cf::rag::error_codes::kDimensionMismatch);
}
#endif
