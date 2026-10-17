// Unit tests for the pattern catalog (design section 6.2).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/rag/catalog.hpp"

namespace {

using cf::rag::load_pattern_catalog;
using cf::rag::parse_pattern_entry;
using cf::rag::pattern_document;

const char* kEntry = R"yaml(
id: PAT-STRATEGY
name: Strategy
level: component
use_when:
  - "several interchangeable algorithms"
avoid_when:
  - "only one implementation is foreseeable"
company_notes: "Prefer std::function over virtual inheritance."
reference_impl: "repo://libs/pricing/strategy/"
conformance_checks:
  cpp: cf-pattern-strategy
  python: swt.pattern.strategy
)yaml";

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_catalog_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

TEST(PatternCatalogTest, ParsesAnEntry) {
  auto entry = parse_pattern_entry(kEntry, "strategy.yaml");
  ASSERT_TRUE(entry.has_value()) << entry.error().message;
  EXPECT_EQ(entry->id, "PAT-STRATEGY");
  EXPECT_EQ(entry->name, "Strategy");
  EXPECT_EQ(entry->level, "component");
  EXPECT_EQ(entry->use_when, (std::vector<std::string>{"several interchangeable algorithms"}));
  EXPECT_EQ(entry->avoid_when,
            (std::vector<std::string>{"only one implementation is foreseeable"}));
  EXPECT_NE(entry->company_notes.find("std::function"), std::string::npos);
  EXPECT_EQ(entry->conformance_checks.at("cpp"), "cf-pattern-strategy");
  EXPECT_EQ(entry->conformance_checks.at("python"), "swt.pattern.strategy");

  const std::string markdown = entry->to_markdown();
  EXPECT_NE(markdown.find("# PAT-STRATEGY — Strategy"), std::string::npos);
  EXPECT_NE(markdown.find("Use when:"), std::string::npos);
  EXPECT_NE(markdown.find("cpp: cf-pattern-strategy"), std::string::npos);
}

TEST(PatternCatalogTest, RejectsInvalidEntries) {
  auto missing_id = parse_pattern_entry("name: X\nlevel: component\n", "x.yaml");
  EXPECT_FALSE(missing_id.has_value());
  EXPECT_EQ(missing_id.error().code, "RAG_CATALOG_PARSE_FAILED");

  auto bad_level = parse_pattern_entry("id: PAT-X\nname: X\nlevel: module\n", "x.yaml");
  EXPECT_FALSE(bad_level.has_value());

  auto bad_list =
      parse_pattern_entry("id: PAT-X\nname: X\nlevel: component\nuse_when: nope\n",
                          "x.yaml");
  EXPECT_FALSE(bad_list.has_value());
}

TEST(PatternCatalogTest, LoadsADirectoryAndRejectsDuplicates) {
  const auto dir = make_temp_dir();
  write_file(dir / "a.yaml", kEntry);
  write_file(dir / "b.yml", R"yaml(
id: PAT-RAII
name: RAII wrapper
level: component
use_when: ["a resource must be released on every path"]
)yaml");

  auto catalog = load_pattern_catalog(dir);
  ASSERT_TRUE(catalog.has_value()) << catalog.error().message;
  ASSERT_EQ(catalog->size(), 2u);
  EXPECT_EQ((*catalog)[0].id, "PAT-STRATEGY");  // sorted by filename: a.yaml, b.yml
  EXPECT_EQ((*catalog)[1].id, "PAT-RAII");

  write_file(dir / "c.yaml", kEntry);  // duplicate PAT-STRATEGY
  auto duplicate = load_pattern_catalog(dir);
  EXPECT_FALSE(duplicate.has_value());
  EXPECT_NE(duplicate.error().message.find("duplicate pattern id"),
            std::string::npos);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(PatternCatalogTest, EntriesBecomeRetrievablePatternDocuments) {
  auto entry = parse_pattern_entry(kEntry, "strategy.yaml");
  ASSERT_TRUE(entry.has_value());
  const cf::rag::Document document = pattern_document(*entry);
  EXPECT_EQ(document.doc_id, "pattern-PAT-STRATEGY");
  EXPECT_EQ(document.type, cf::rag::DocType::Pattern);
  EXPECT_EQ(document.status, "active");
  EXPECT_EQ(document.scope, "company");
  EXPECT_NE(document.text.find("PAT-STRATEGY"), std::string::npos);
}

}  // namespace
