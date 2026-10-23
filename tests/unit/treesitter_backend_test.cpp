// Unit tests for the tree-sitter C back end (design section 6.5): symbol
// extraction, call edges, includes, doc comments, incremental update and the
// architecture layering check.
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

using cf::codeintel::ArchitectureRules;
using cf::codeintel::CallEdge;
using cf::codeintel::IncludeEdge;
using cf::codeintel::LayerRule;
using cf::codeintel::Symbol;
using cf::codeintel::SymbolGraph;
using cf::codeintel::SymbolKind;
using cf::codeintel::TreeSitterBackend;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_treesitter_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const char* kUtilH = R"c(#ifndef UTIL_H
#define UTIL_H

/** Adds two integers. */
int add(int a, int b);

struct Point {
  int x;
  int y;
};

enum Color { RED, GREEN };

typedef unsigned long size_alias;

#define MAX_SIZE 128
#define SQR(x) ((x) * (x))

extern int global_counter;

struct Ops { int (*run)(int); };

#endif
)c";

const char* kUtilC = R"c(#include "util.h"
#include <stdio.h>

int add(int a, int b) {
  return a + b;
}

static int twice(int v) {
  return add(v, v);
}

int compute(int x) {
  int y = twice(x);
  printf("%d\n", y);
  return y;
}

int dispatch(struct Ops* ops, int v) {
  return ops->run(v);
}
)c";

const char* kMainC = R"c(#include "util.h"

int main(void) {
  return compute(40) + add(1, 2);
}
)c";

class TreeSitterBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    write_file(dir_ / "util.h", kUtilH);
    write_file(dir_ / "util.c", kUtilC);
    write_file(dir_ / "main.c", kMainC);
    // Non-source files are ignored by the back end.
    write_file(dir_ / "README.md", "# not code\n");
    backend_ = cf::codeintel::make_c_backend();
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

TEST_F(TreeSitterBackendTest, IndexesTheRecognisedSourceFiles) {
  EXPECT_EQ(backend_->language(), "c");
  ASSERT_EQ(graph_.files.size(), 3u);
  EXPECT_EQ(graph_.files.at(0).path, "main.c");  // sorted by path
  EXPECT_EQ(graph_.files.at(1).path, "util.c");
  EXPECT_EQ(graph_.files.at(2).path, "util.h");
  for (const auto& file : graph_.files) {
    EXPECT_EQ(file.sha256.size(), 64u);
  }
}

TEST_F(TreeSitterBackendTest, FindsDefinitionsAndPrototypes) {
  const auto add = symbols_named("add");
  ASSERT_EQ(add.size(), 2u);
  const Symbol* definition = add.at(0)->is_definition ? add.at(0) : add.at(1);
  const Symbol* prototype = add.at(0)->is_definition ? add.at(1) : add.at(0);
  EXPECT_EQ(definition->file, "util.c");
  EXPECT_EQ(definition->line, 4u);
  EXPECT_EQ(definition->kind, SymbolKind::Function);
  EXPECT_EQ(definition->signature, "int add(int a, int b)");
  EXPECT_EQ(prototype->file, "util.h");
  EXPECT_EQ(prototype->line, 5u);
  EXPECT_FALSE(prototype->is_definition);

  const auto twice = symbols_named("twice");  // static: still indexed
  ASSERT_EQ(twice.size(), 1u);
  EXPECT_TRUE(twice.at(0)->is_definition);
  EXPECT_EQ(twice.at(0)->line, 8u);
}

TEST_F(TreeSitterBackendTest, ExtractsDocComments) {
  const auto add = symbols_named("add");
  const Symbol* prototype =
      add.at(0)->is_definition ? add.at(1) : add.at(0);
  EXPECT_EQ(prototype->doc, "Adds two integers.");
}

TEST_F(TreeSitterBackendTest, IndexesTypesMacrosAndVariables) {
  const auto point = symbols_named("Point");
  ASSERT_EQ(point.size(), 1u);
  EXPECT_EQ(point.at(0)->kind, SymbolKind::Struct);
  EXPECT_EQ(point.at(0)->line, 7u);

  const auto color = symbols_named("Color");
  ASSERT_EQ(color.size(), 1u);
  EXPECT_EQ(color.at(0)->kind, SymbolKind::Enum);

  const auto alias = symbols_named("size_alias");
  ASSERT_EQ(alias.size(), 1u);
  EXPECT_EQ(alias.at(0)->kind, SymbolKind::Typedef);

  const auto max_size = symbols_named("MAX_SIZE");
  ASSERT_EQ(max_size.size(), 1u);
  EXPECT_EQ(max_size.at(0)->kind, SymbolKind::Macro);
  const auto sqr = symbols_named("SQR");  // function-like macro
  ASSERT_EQ(sqr.size(), 1u);
  EXPECT_EQ(sqr.at(0)->kind, SymbolKind::Macro);

  const auto counter = symbols_named("global_counter");
  ASSERT_EQ(counter.size(), 1u);
  EXPECT_EQ(counter.at(0)->kind, SymbolKind::Variable);

  const auto ops = symbols_named("Ops");
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_EQ(ops.at(0)->kind, SymbolKind::Struct);

  // A function pointer field is not a function symbol.
  EXPECT_TRUE(symbols_named("run").empty());
}

TEST_F(TreeSitterBackendTest, RecordsCallEdgesWithCallers) {
  {
    const auto callers = calls_to("add");
    ASSERT_EQ(callers.size(), 2u);
    std::vector<std::string> names;
    for (const auto& edge : callers) {
      names.push_back(edge.caller);
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"main", "twice"}));
  }
  EXPECT_EQ(calls_to("twice").size(), 1u);
  EXPECT_EQ(calls_to("twice").at(0).caller, "compute");
  EXPECT_EQ(calls_to("compute").size(), 1u);
  EXPECT_EQ(calls_to("compute").at(0).caller, "main");
  {
    // printf is an external callee; the edge still records the caller.
    const auto calls = calls_to("printf");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.at(0).caller, "compute");
    EXPECT_EQ(calls.at(0).file, "util.c");
    EXPECT_EQ(calls.at(0).line, 14u);
  }
  {
    // Calls through a member function pointer record the field name.
    const auto calls = calls_to("run");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.at(0).caller, "dispatch");
  }
}

TEST_F(TreeSitterBackendTest, RecordsLocalAndSystemIncludes) {
  ASSERT_EQ(graph_.includes.size(), 3u);
  std::vector<IncludeEdge> util_c;
  for (const auto& include : graph_.includes) {
    if (include.file == "util.c") {
      util_c.push_back(include);
    }
  }
  ASSERT_EQ(util_c.size(), 2u);
  EXPECT_EQ(util_c.at(0).included, "util.h");
  EXPECT_FALSE(util_c.at(0).system);
  EXPECT_EQ(util_c.at(0).line, 1u);
  EXPECT_EQ(util_c.at(1).included, "stdio.h");
  EXPECT_TRUE(util_c.at(1).system);
}

TEST_F(TreeSitterBackendTest, UpdateReparsesOnlyTheChangedFiles) {
  write_file(dir_ / "util.c", std::string(kUtilC) +
                                  "\nint extra(void) {\n  return add(0, 0);\n}\n");
  auto updated = backend_->update(graph_, {"util.c"});
  ASSERT_TRUE(updated.has_value()) << updated.error().message;
  ASSERT_EQ(updated->files.size(), 1u);
  EXPECT_EQ(updated->files.at(0).path, "util.c");
  bool found_extra = false;
  for (const auto& symbol : updated->symbols) {
    if (symbol.name == "extra") {
      found_extra = true;
    }
  }
  EXPECT_TRUE(found_extra);
}

TEST_F(TreeSitterBackendTest, UpdateIgnoresForeignExtensions) {
  auto updated = backend_->update(graph_, {"README.md"});
  ASSERT_TRUE(updated.has_value()) << updated.error().message;
  EXPECT_TRUE(updated->files.empty());
}

TEST_F(TreeSitterBackendTest, IndexOnAMissingDirectoryFails) {
  auto result = backend_->index(dir_ / "nope");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, cf::codeintel::error_codes::kParseFailed);
}

TEST_F(TreeSitterBackendTest, CheckArchitectureFlagsLayerViolations) {
  std::filesystem::path project = dir_ / "layers";
  write_file(project / "src" / "domain" / "order.c",
             "#include \"infra/db.h\"\n#include <infra/sys.h>\n");
  write_file(project / "src" / "infra" / "db.c", "#include \"domain/order.h\"\n");
  auto indexed = backend_->index(project);
  ASSERT_TRUE(indexed.has_value()) << indexed.error().message;

  ArchitectureRules rules;
  rules.layers = {LayerRule{"ARCH-1", "src/domain", "infra/"}};
  auto diagnostics = backend_->check_architecture(*indexed, rules);
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  // The local include crosses the layer; the system include does not count
  // and neither does the infra file (the rule covers domain sources only).
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ(diagnostics->at(0).rule_id, "ARCH-1");
  EXPECT_EQ(diagnostics->at(0).file, "src/domain/order.c");
  EXPECT_EQ(diagnostics->at(0).line, 1u);
}

}  // namespace
