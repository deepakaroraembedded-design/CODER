// Unit tests for the context packer (design section 6.5): ranked packing of
// the seed symbols, their dependencies, callers and file siblings within a
// token budget.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "cf/codeintel/context_packer.hpp"

namespace {

using cf::codeintel::CallEdge;
using cf::codeintel::ContextPacker;
using cf::codeintel::ContextRequest;
using cf::codeintel::FileIndex;
using cf::codeintel::FileRecord;
using cf::codeintel::Symbol;
using cf::codeintel::SymbolIndex;
using cf::codeintel::SymbolKind;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_packer_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

Symbol function(std::string name, std::string file, std::uint32_t line,
                std::string signature = "", std::string doc = "",
                bool is_definition = true) {
  Symbol symbol;
  symbol.name = std::move(name);
  symbol.qualified = symbol.name;
  symbol.kind = SymbolKind::Function;
  symbol.file = std::move(file);
  symbol.line = line;
  symbol.signature = std::move(signature);
  symbol.doc = std::move(doc);
  symbol.is_definition = is_definition;
  return symbol;
}

class ContextPackerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = SymbolIndex::open(dir_ / "index.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    index_ = std::move(*opened);

    FileIndex header;
    header.record = FileRecord{"src/registry.h", "h1"};
    header.symbols = {
        function("registry_open", "src/registry.h", 7,
                 "int registry_open(const char* path)", "Opens the registry.",
                 /*is_definition=*/false),
    };
    ASSERT_TRUE(index_->replace_file(header).has_value());

    FileIndex impl;
    impl.record = FileRecord{"src/registry.c", "h2"};
    impl.symbols = {
        function("registry_open", "src/registry.c", 12,
                 "int registry_open(const char* path)"),
        function("registry_close", "src/registry.c", 20,
                 "void registry_close(void)"),
    };
    impl.calls = {
        CallEdge{"registry_open", "log_open", "src/registry.c", 14},
        CallEdge{"registry_open", "printf", "src/registry.c", 15},
        CallEdge{"registry_close", "registry_flush", "src/registry.c", 22},
    };
    ASSERT_TRUE(index_->replace_file(impl).has_value());

    FileIndex log;
    log.record = FileRecord{"src/log.c", "h3"};
    log.symbols = {function("log_open", "src/log.c", 4, "void log_open(void)")};
    ASSERT_TRUE(index_->replace_file(log).has_value());

    FileIndex main_file;
    main_file.record = FileRecord{"src/main.c", "h4"};
    main_file.symbols = {function("main", "src/main.c", 2, "int main(void)")};
    main_file.calls = {CallEdge{"main", "registry_open", "src/main.c", 5}};
    ASSERT_TRUE(index_->replace_file(main_file).has_value());

    packer_ = std::make_unique<ContextPacker>(*index_);
  }

  void TearDown() override {
    packer_.reset();
    index_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  ContextRequest for_seed(std::string seed, std::uint32_t budget = 4000) {
    ContextRequest request;
    request.seeds = {std::move(seed)};
    request.token_budget = budget;
    return request;
  }

  std::filesystem::path dir_;
  std::unique_ptr<SymbolIndex> index_;
  std::unique_ptr<ContextPacker> packer_;
};

TEST(EstimateTokensTest, RoundsUpAtFourCharactersPerToken) {
  EXPECT_EQ(cf::codeintel::estimate_tokens(""), 0u);
  EXPECT_EQ(cf::codeintel::estimate_tokens("abc"), 1u);
  EXPECT_EQ(cf::codeintel::estimate_tokens("abcd"), 1u);
  EXPECT_EQ(cf::codeintel::estimate_tokens("abcde"), 2u);
}

TEST_F(ContextPackerTest, PacksSeedsDependenciesCallersAndSiblings) {
  auto packed = packer_->pack(for_seed("registry_open"));
  ASSERT_TRUE(packed.has_value()) << packed.error().message;

  const std::string& text = packed->text;
  // The seed's definition and prototype (same name, two rows).
  EXPECT_NE(text.find("function registry_open (src/registry.c:12)"),
            std::string::npos);
  EXPECT_NE(text.find("function registry_open (src/registry.h:7)"),
            std::string::npos);
  EXPECT_NE(text.find("Opens the registry."), std::string::npos);
  // Direct dependency, resolved to its signature.
  EXPECT_NE(text.find("function log_open (src/log.c:4)"), std::string::npos);
  EXPECT_NE(text.find("void log_open(void)"), std::string::npos);
  // The caller, resolved to its signature.
  EXPECT_NE(text.find("function main (src/main.c:2)"), std::string::npos);
  // The file sibling.
  EXPECT_NE(text.find("function registry_close (src/registry.c:20)"),
            std::string::npos);

  // Ranked: seed < dependency < caller < sibling (design section 6.5).
  const auto at = [&text](const std::string& needle) {
    return text.find(needle);
  };
  EXPECT_LT(at("registry_open (src/registry.c:12)"), at("log_open"));
  EXPECT_LT(at("log_open"), at("main (src/main.c:2)"));
  EXPECT_LT(at("main (src/main.c:2)"), at("registry_close"));

  // No duplicates: a symbol reached twice is packed once.
  EXPECT_EQ(packed->included.size(),
            std::set<std::string>(packed->included.begin(),
                                  packed->included.end()).size());
  EXPECT_EQ(packed->tokens, cf::codeintel::estimate_tokens(packed->text));
}

TEST_F(ContextPackerTest, UnresolvedCallTargetsGetOneLiners) {
  auto packed = packer_->pack(for_seed("registry_open"));
  ASSERT_TRUE(packed.has_value()) << packed.error().message;
  // printf is not in the index: the call site is still noted.
  EXPECT_NE(packed->text.find("calls printf (src/registry.c:15)"),
            std::string::npos);
}

TEST_F(ContextPackerTest, SeedsResolveByPathAndDirectoryPrefix) {
  auto by_file = packer_->pack(for_seed("src/registry.h"));
  ASSERT_TRUE(by_file.has_value()) << by_file.error().message;
  EXPECT_NE(by_file->text.find("registry_open"), std::string::npos);
  EXPECT_TRUE(by_file->unresolved.empty());

  auto by_dir = packer_->pack(for_seed("src/registry"));
  ASSERT_TRUE(by_dir.has_value()) << by_dir.error().message;
  EXPECT_TRUE(by_dir->unresolved.empty());
  EXPECT_NE(by_dir->text.find("registry_open"), std::string::npos);
  EXPECT_NE(by_dir->text.find("registry_close"), std::string::npos);

  // A prefix that is not on a path boundary must not match.
  auto not_boundary = packer_->pack(for_seed("src/reg"));
  ASSERT_TRUE(not_boundary.has_value()) << not_boundary.error().message;
  EXPECT_TRUE(not_boundary->included.empty());
  EXPECT_EQ(not_boundary->unresolved.size(), 1u);
}

TEST_F(ContextPackerTest, TheBudgetCutsLowerRankedEntries) {
  auto full = packer_->pack(for_seed("registry_open"));
  ASSERT_TRUE(full.has_value()) << full.error().message;
  ASSERT_FALSE(full->included.empty());

  // One token short of the full pack: something must be cut.
  auto tight = packer_->pack(for_seed("registry_open", full->tokens - 1));
  ASSERT_TRUE(tight.has_value()) << tight.error().message;
  EXPECT_FALSE(tight->dropped.empty());
  EXPECT_LE(tight->tokens, full->tokens);
  EXPECT_LT(tight->included.size(), full->included.size());

  // A zero budget packs nothing.
  auto nothing = packer_->pack(for_seed("registry_open", 0));
  ASSERT_TRUE(nothing.has_value()) << nothing.error().message;
  EXPECT_TRUE(nothing->included.empty());
  EXPECT_TRUE(nothing->text.empty());
}

TEST_F(ContextPackerTest, UnresolvedSeedsAreReported) {
  auto packed = packer_->pack(for_seed("no_such_thing"));
  ASSERT_TRUE(packed.has_value()) << packed.error().message;
  EXPECT_TRUE(packed->included.empty());
  EXPECT_TRUE(packed->text.empty());
  ASSERT_EQ(packed->unresolved.size(), 1u);
  EXPECT_EQ(packed->unresolved.at(0), "no_such_thing");
}

}  // namespace
