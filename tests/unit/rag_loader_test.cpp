// Unit tests for the RAG document loader (design section 6.3, ingestion).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/rag/loader.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_loader_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

const char* kDocument = R"yaml(---
doc_id: CG
version: "3.2"
type: guideline
owner: engineering
classification: internal
scope: product
scope_value: product-y
effective_from: 2026-01-01
status: active
---
# Naming

Use snake_case for functions.
)yaml";

TEST(RagLoaderTest, LoadsFrontMatterDocument) {
  auto document = cf::rag::load_document_string(kDocument, "docs/cg.md");
  ASSERT_TRUE(document.has_value()) << document.error().message;
  EXPECT_EQ(document->doc_id, "CG");
  EXPECT_EQ(document->version, "3.2");
  EXPECT_EQ(document->type, cf::rag::DocType::Guideline);
  EXPECT_EQ(document->owner, "engineering");
  EXPECT_EQ(document->classification, "internal");
  EXPECT_EQ(document->scope, "product");
  EXPECT_EQ(document->scope_value, "product-y");
  EXPECT_EQ(document->status, "active");
  EXPECT_EQ(document->source, "docs/cg.md");
  EXPECT_TRUE(document->topic.empty());  // no topic in the front matter
  EXPECT_NE(document->text.find("snake_case"), std::string::npos);
  EXPECT_GT(document->effective_from, 0);
}

TEST(RagLoaderTest, ParsesTheConflictTopic) {
  const std::string text = R"yaml(---
doc_id: REQ-COLOR-A
version: "1.0"
type: requirements
topic: REQ-COLOR
status: active
---
# Colour

The status colour must be green.
)yaml";
  auto document = cf::rag::load_document_string(text, "docs/colour.md");
  ASSERT_TRUE(document.has_value()) << document.error().message;
  EXPECT_EQ(document->topic, "REQ-COLOR");
}

TEST(RagLoaderTest, EpochDatesAreAccepted) {
  const std::string text = R"yaml(---
doc_id: P
version: "1"
type: policy
effective_from: 1700000000
effective_to: 1800000000
---
Body text.
)yaml";
  auto document = cf::rag::load_document_string(text, "p.md");
  ASSERT_TRUE(document.has_value()) << document.error().message;
  EXPECT_EQ(document->effective_from, 1700000000);
  EXPECT_EQ(document->effective_to, 1800000000);
}

TEST(RagLoaderTest, MissingFrontMatterFails) {
  auto document = cf::rag::load_document_string("# No front matter\n", "x.md");
  ASSERT_FALSE(document.has_value());
  EXPECT_EQ(document.error().code, cf::rag::error_codes::kInvalidDocument);
}

TEST(RagLoaderTest, UnterminatedFrontMatterFails) {
  auto document =
      cf::rag::load_document_string("---\ndoc_id: X\nversion: \"1\"\n", "x.md");
  ASSERT_FALSE(document.has_value());
  EXPECT_EQ(document.error().code, cf::rag::error_codes::kInvalidDocument);
}

TEST(RagLoaderTest, UnknownTypeFails) {
  const std::string text = R"yaml(---
doc_id: B
version: "1"
type: blog
---
Body.
)yaml";
  auto document = cf::rag::load_document_string(text, "b.md");
  ASSERT_FALSE(document.has_value());
  EXPECT_EQ(document.error().code, cf::rag::error_codes::kInvalidDocument);
}

TEST(RagLoaderTest, MissingRequiredFieldFails) {
  const std::string text = R"yaml(---
doc_id: B
type: policy
---
Body.
)yaml";
  auto document = cf::rag::load_document_string(text, "b.md");
  ASSERT_FALSE(document.has_value());
  EXPECT_EQ(document.error().code, cf::rag::error_codes::kInvalidDocument);
}

TEST(RagLoaderTest, EmptyBodyFails) {
  const std::string text = R"yaml(---
doc_id: B
version: "1"
type: policy
---
)yaml";
  auto document = cf::rag::load_document_string(text, "b.md");
  ASSERT_FALSE(document.has_value());
  EXPECT_EQ(document.error().code, cf::rag::error_codes::kInvalidDocument);
}

TEST(RagLoaderTest, LoadDirLoadsSortedFiles) {
  const auto dir = make_temp_dir();
  {
    std::ofstream out(dir / "b.md");
    out << "---\ndoc_id: B\nversion: \"1\"\ntype: policy\n---\nBody B.\n";
  }
  {
    std::ofstream out(dir / "a.md");
    out << "---\ndoc_id: A\nversion: \"1\"\ntype: guideline\n---\nBody A.\n";
  }
  auto documents = cf::rag::load_document_dir(dir);
  ASSERT_TRUE(documents.has_value()) << documents.error().message;
  ASSERT_EQ(documents->size(), 2u);
  EXPECT_EQ((*documents)[0].doc_id, "A");  // sorted by filename
  EXPECT_EQ((*documents)[1].doc_id, "B");

  {
    std::ofstream out(dir / "bad.md");
    out << "no front matter\n";
  }
  auto broken = cf::rag::load_document_dir(dir);
  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().code, cf::rag::error_codes::kInvalidDocument);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
