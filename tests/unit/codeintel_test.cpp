// Unit tests for cf-codeintel: the back-end registry, the SQLite symbol
// index and the indexer glue (design section 6.5).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/codeintel/backend.hpp"
#include "cf/codeintel/indexer.hpp"
#include "cf/codeintel/symbol_index.hpp"

namespace {

using cf::codeintel::ArchitectureRules;
using cf::codeintel::BackendRegistry;
using cf::codeintel::CallEdge;
using cf::codeintel::FileIndex;
using cf::codeintel::FileRecord;
using cf::codeintel::LanguageBackend;
using cf::codeintel::Symbol;
using cf::codeintel::SymbolGraph;
using cf::codeintel::SymbolIndex;
using cf::codeintel::SymbolKind;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_codeintel_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

// A canned-graph back end for the registry and indexer tests; the real
// parsing is covered by the tree-sitter back-end tests.
class FakeBackend final : public LanguageBackend {
 public:
  std::string_view language() const override { return "fake"; }

  cf::Result<SymbolGraph> index(const std::filesystem::path& root) override {
    SymbolGraph graph;
    graph.root = root.string();
    graph.language = "fake";
    add_file(graph, "a.fake", {"alpha", "shared"});
    add_file(graph, "b.fake", {"beta", "shared"});
    return graph;
  }

  cf::Result<SymbolGraph> update(
      const SymbolGraph& graph,
      const std::vector<std::filesystem::path>& changed) override {
    SymbolGraph delta;
    delta.root = graph.root;
    delta.language = "fake";
    for (const auto& path : changed) {
      add_file(delta, path.string(), {"updated_" + path.stem().string()});
    }
    return delta;
  }

  cf::Result<std::vector<cf::codeintel::Diagnostic>> check_architecture(
      const SymbolGraph& /*graph*/, const ArchitectureRules& /*rules*/) override {
    return std::vector<cf::codeintel::Diagnostic>{};
  }

  // Every canned file defines one function per name and calls "shared".
  static void add_file(SymbolGraph& graph, const std::string& path,
                       std::vector<std::string> functions) {
    graph.files.push_back(FileRecord{path, "hash-" + path});
    for (const auto& name : functions) {
      Symbol symbol;
      symbol.name = name;
      symbol.qualified = name;
      symbol.kind = SymbolKind::Function;
      symbol.file = path;
      symbol.line = 1;
      symbol.signature = "void " + name + "(void)";
      symbol.is_definition = true;
      graph.symbols.push_back(std::move(symbol));
    }
    graph.calls.push_back(CallEdge{functions.front(), "shared", path, 2});
  }
};

class SymbolIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = SymbolIndex::open(dir_ / "index.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    index_ = std::move(*opened);
  }

  void TearDown() override {
    index_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::unique_ptr<SymbolIndex> index_;
};

TEST(BackendRegistryTest, FindsTheRegisteredBackend) {
  BackendRegistry registry;
  registry.add(std::make_unique<FakeBackend>());
  auto backend = registry.for_language("fake");
  ASSERT_TRUE(backend.has_value()) << backend.error().message;
  EXPECT_EQ((*backend)->language(), "fake");
}

TEST(BackendRegistryTest, UnknownLanguageIsAnError) {
  BackendRegistry registry;
  registry.add(std::make_unique<FakeBackend>());
  auto backend = registry.for_language("cobol");
  ASSERT_FALSE(backend.has_value());
  EXPECT_EQ(backend.error().code, cf::codeintel::error_codes::kUnknownLanguage);
}

TEST_F(SymbolIndexTest, FindSymbolMatchesNameAndQualifiedName) {
  FileIndex file;
  file.record = FileRecord{"src/registry.c", "h1"};
  Symbol declaration;
  declaration.name = "registry_open";
  declaration.qualified = "registry_open";
  declaration.kind = SymbolKind::Function;
  declaration.file = "src/registry.c";
  declaration.line = 7;
  declaration.signature = "int registry_open(const char* path)";
  declaration.is_definition = false;
  Symbol definition = declaration;
  definition.line = 42;
  definition.is_definition = true;
  file.symbols = {declaration, definition};
  file.calls = {CallEdge{"main", "registry_open", "src/registry.c", 90}};
  ASSERT_TRUE(index_->replace_file(file).has_value());

  auto by_name = index_->find_symbol("registry_open");
  ASSERT_TRUE(by_name.has_value()) << by_name.error().message;
  ASSERT_EQ(by_name->size(), 2u);
  // Definitions sort before declarations.
  EXPECT_TRUE(by_name->at(0).is_definition);
  EXPECT_EQ(by_name->at(0).line, 42u);
  EXPECT_FALSE(by_name->at(1).is_definition);

  auto missing = index_->find_symbol("does_not_exist");
  ASSERT_TRUE(missing.has_value());
  EXPECT_TRUE(missing->empty());
}

TEST_F(SymbolIndexTest, GetCallersReturnsTheRecordedEdges) {
  FileIndex a;
  a.record = FileRecord{"a.c", "h1"};
  a.calls = {CallEdge{"main", "shared", "a.c", 3}};
  FileIndex b;
  b.record = FileRecord{"b.c", "h2"};
  b.calls = {CallEdge{"helper", "shared", "b.c", 8}};
  ASSERT_TRUE(index_->replace_file(a).has_value());
  ASSERT_TRUE(index_->replace_file(b).has_value());

  auto callers = index_->get_callers("shared");
  ASSERT_TRUE(callers.has_value()) << callers.error().message;
  ASSERT_EQ(callers->size(), 2u);
  EXPECT_EQ(callers->at(0).caller, "main");
  EXPECT_EQ(callers->at(0).file, "a.c");
  EXPECT_EQ(callers->at(1).caller, "helper");
}

TEST_F(SymbolIndexTest, ReplaceFileDropsTheOldRows) {
  FileIndex file;
  file.record = FileRecord{"a.c", "h1"};
  Symbol stale;
  stale.name = "stale_name";
  stale.qualified = "stale_name";
  stale.file = "a.c";
  stale.line = 1;
  file.symbols = {stale};
  file.calls = {CallEdge{"stale_name", "shared", "a.c", 2}};
  ASSERT_TRUE(index_->replace_file(file).has_value());

  FileIndex updated;
  updated.record = FileRecord{"a.c", "h2"};
  Symbol fresh = stale;
  fresh.name = "fresh_name";
  fresh.qualified = "fresh_name";
  updated.symbols = {fresh};
  ASSERT_TRUE(index_->replace_file(updated).has_value());

  auto stale_hits = index_->find_symbol("stale_name");
  ASSERT_TRUE(stale_hits.has_value());
  EXPECT_TRUE(stale_hits->empty());
  auto fresh_hits = index_->find_symbol("fresh_name");
  ASSERT_TRUE(fresh_hits.has_value());
  EXPECT_EQ(fresh_hits->size(), 1u);
  // The replaced file's call edges went with it.
  auto callers = index_->get_callers("shared");
  ASSERT_TRUE(callers.has_value());
  EXPECT_TRUE(callers->empty());
}

TEST_F(SymbolIndexTest, RemoveFileDropsEverything) {
  FileIndex file;
  file.record = FileRecord{"a.c", "h1"};
  Symbol symbol;
  symbol.name = "alpha";
  symbol.qualified = "alpha";
  symbol.file = "a.c";
  symbol.line = 1;
  file.symbols = {symbol};
  ASSERT_TRUE(index_->replace_file(file).has_value());

  ASSERT_TRUE(index_->remove_file("a.c").has_value());
  auto hits = index_->find_symbol("alpha");
  ASSERT_TRUE(hits.has_value());
  EXPECT_TRUE(hits->empty());
}

TEST_F(SymbolIndexTest, IndexTreeWritesTheBackendGraph) {
  FakeBackend backend;
  auto indexed = cf::codeintel::index_tree(backend, *index_, dir_);
  ASSERT_TRUE(indexed.has_value()) << indexed.error().message;
  EXPECT_EQ(*indexed, 2u);

  auto alpha = index_->find_symbol("alpha");
  ASSERT_TRUE(alpha.has_value());
  ASSERT_EQ(alpha->size(), 1u);
  EXPECT_EQ(alpha->at(0).file, "a.fake");

  auto shared = index_->find_symbol("shared");
  ASSERT_TRUE(shared.has_value());
  EXPECT_EQ(shared->size(), 2u);  // one definition per canned file

  auto callers = index_->get_callers("shared");
  ASSERT_TRUE(callers.has_value());
  EXPECT_EQ(callers->size(), 2u);  // and one call edge per file
}

TEST_F(SymbolIndexTest, UpdateFilesReplacesChangedAndRemovesDeleted) {
  FakeBackend backend;
  ASSERT_TRUE(cf::codeintel::index_tree(backend, *index_, dir_).has_value());

  // a.fake changed on disk; b.fake was deleted.
  std::ofstream out(dir_ / "a.fake");
  out << "changed\n";
  out.close();

  auto updated = cf::codeintel::update_files(backend, *index_, dir_,
                                             {"a.fake", "b.fake"});
  ASSERT_TRUE(updated.has_value()) << updated.error().message;
  EXPECT_EQ(*updated, 2u);

  // The changed file now carries the update's rows only.
  auto stale = index_->find_symbol("alpha");
  ASSERT_TRUE(stale.has_value());
  EXPECT_TRUE(stale->empty());
  auto fresh = index_->find_symbol("updated_a");
  ASSERT_TRUE(fresh.has_value());
  EXPECT_EQ(fresh->size(), 1u);

  // The deleted file's rows are gone.
  auto removed = index_->find_symbol("beta");
  ASSERT_TRUE(removed.has_value());
  EXPECT_TRUE(removed->empty());
}

}  // namespace
