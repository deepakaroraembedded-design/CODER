// Unit tests for the tree-sitter Go back end (design section 6.5):
// functions, methods (qualified Receiver.Method), types, package-level
// vars/consts, imports, doc comments and call edges with caller attribution.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/codeintel/treesitter_backend.hpp"

namespace {

using cf::codeintel::CallEdge;
using cf::codeintel::Symbol;
using cf::codeintel::SymbolGraph;
using cf::codeintel::SymbolKind;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_ts_go_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const char* kRegistryGo = R"go(package registry

import (
	"fmt"
	"github.com/example/infra/db"
)

const maxRetries = 3

var defaultTimeout int

// Opener opens the registry.
type Opener interface {
	Open(path string) error
}

// Point is a pair.
type Point struct {
	x, y int
}

type Name = string

// Start opens the registry.
func (r *Registry) Start(path string) error {
	db.Open(path)
	return nil
}

func helper() {
	fmt.Println("x")
}
)go";

class TreeSitterGoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    write_file(dir_ / "registry.go", kRegistryGo);
    write_file(dir_ / "README.md", "# not code\n");
    backend_ = cf::codeintel::make_go_backend();
    auto indexed = backend_->index(dir_);
    ASSERT_TRUE(indexed.has_value()) << indexed.error().message;
    graph_ = std::move(*indexed);
  }

  void TearDown() override {
    backend_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::vector<const Symbol*> symbols_named(const std::string& name) const {
    std::vector<const Symbol*> out;
    for (const auto& symbol : graph_.symbols) {
      if (symbol.name == name) {
        out.push_back(&symbol);
      }
    }
    return out;
  }

  std::vector<CallEdge> calls_to(const std::string& callee) const {
    std::vector<CallEdge> out;
    for (const auto& edge : graph_.calls) {
      if (edge.callee == callee) {
        out.push_back(edge);
      }
    }
    return out;
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::codeintel::LanguageBackend> backend_;
  SymbolGraph graph_;
};

TEST_F(TreeSitterGoTest, IndexesOnlyGoFiles) {
  EXPECT_EQ(backend_->language(), "go");
  ASSERT_EQ(graph_.files.size(), 1u);
  EXPECT_EQ(graph_.files.at(0).path, "registry.go");
}

TEST_F(TreeSitterGoTest, IndexesFunctionsMethodsAndTypes) {
  const auto helper = symbols_named("helper");
  ASSERT_EQ(helper.size(), 1u);
  EXPECT_EQ(helper.at(0)->kind, SymbolKind::Function);
  EXPECT_EQ(helper.at(0)->line, 30u);
  EXPECT_EQ(helper.at(0)->signature, "func helper()");

  const auto start = symbols_named("Start");
  ASSERT_EQ(start.size(), 1u);
  EXPECT_EQ(start.at(0)->kind, SymbolKind::Method);
  EXPECT_EQ(start.at(0)->qualified, "Registry.Start");
  EXPECT_EQ(start.at(0)->line, 25u);
  EXPECT_EQ(start.at(0)->doc, "Start opens the registry.");
  EXPECT_EQ(start.at(0)->signature, "func (r *Registry) Start(path string) error");

  const auto opener = symbols_named("Opener");
  ASSERT_EQ(opener.size(), 1u);
  EXPECT_EQ(opener.at(0)->kind, SymbolKind::Interface);
  EXPECT_EQ(opener.at(0)->line, 13u);
  EXPECT_EQ(opener.at(0)->doc, "Opener opens the registry.");

  const auto point = symbols_named("Point");
  ASSERT_EQ(point.size(), 1u);
  EXPECT_EQ(point.at(0)->kind, SymbolKind::Struct);
  EXPECT_EQ(point.at(0)->signature, "type Point struct");

  const auto name = symbols_named("Name");
  ASSERT_EQ(name.size(), 1u);
  EXPECT_EQ(name.at(0)->kind, SymbolKind::Typedef);

  // Struct fields are not symbols.
  EXPECT_TRUE(symbols_named("x").empty());
}

TEST_F(TreeSitterGoTest, IndexesPackageLevelVarsAndConsts) {
  const auto retries = symbols_named("maxRetries");
  ASSERT_EQ(retries.size(), 1u);
  EXPECT_EQ(retries.at(0)->kind, SymbolKind::Variable);
  EXPECT_EQ(retries.at(0)->line, 8u);

  const auto timeout = symbols_named("defaultTimeout");
  ASSERT_EQ(timeout.size(), 1u);
  EXPECT_EQ(timeout.at(0)->kind, SymbolKind::Variable);
  EXPECT_EQ(timeout.at(0)->line, 10u);
}

TEST_F(TreeSitterGoTest, RecordsCallsWithQualifiedCallers) {
  {
    const auto calls = calls_to("Open");  // db.Open -> selector field
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.at(0).caller, "Registry.Start");
    EXPECT_EQ(calls.at(0).line, 26u);
  }
  {
    const auto calls = calls_to("Println");  // fmt.Println
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.at(0).caller, "helper");
  }
}

TEST_F(TreeSitterGoTest, RecordsImportsAsIncludeEdges) {
  std::vector<std::string> modules;
  for (const auto& include : graph_.includes) {
    modules.push_back(include.included);
    EXPECT_EQ(include.file, "registry.go");
    EXPECT_FALSE(include.system);
  }
  std::sort(modules.begin(), modules.end());
  EXPECT_EQ(modules,
            (std::vector<std::string>{"fmt", "github.com/example/infra/db"}));
  EXPECT_EQ(graph_.includes.at(0).line, 4u);
}

TEST_F(TreeSitterGoTest, UpdateReparsesOnlyTheChangedFiles) {
  write_file(dir_ / "extra.go", "package registry\n\nfunc extra() int { return 1 }\n");
  auto updated = backend_->update(graph_, {"extra.go", "README.md"});
  ASSERT_TRUE(updated.has_value()) << updated.error().message;
  ASSERT_EQ(updated->files.size(), 1u);  // README.md is not a Go source
  EXPECT_EQ(updated->files.at(0).path, "extra.go");
  ASSERT_EQ(updated->symbols.size(), 1u);
  EXPECT_EQ(updated->symbols.at(0).name, "extra");
}

}  // namespace
