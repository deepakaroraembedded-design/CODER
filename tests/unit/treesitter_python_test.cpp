// Unit tests for the tree-sitter Python back end (design section 6.5):
// functions, classes and methods, module variables, imports, docstrings and
// call edges with caller attribution.
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
using cf::codeintel::IncludeEdge;
using cf::codeintel::Symbol;
using cf::codeintel::SymbolGraph;
using cf::codeintel::SymbolKind;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_ts_python_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const char* kRegistryPy = R"py("""Registry module."""

import os
import logging
import infra.db as db
from infra.queue import connect

DEFAULT_SIZE = 128


class Registry:
    """The registry of things."""

    def start(self, path):
        """Start the registry."""
        db.open(path)
        self.size = DEFAULT_SIZE

    def stop(self):
        pass


def make_registry(path):
    return Registry()


def helper():
    r = make_registry("x")
    logging.info("made")
)py";

class TreeSitterPythonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    write_file(dir_ / "src" / "registry.py", kRegistryPy);
    write_file(dir_ / "README.md", "# not code\n");
    backend_ = cf::codeintel::make_python_backend();
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

TEST_F(TreeSitterPythonTest, IndexesOnlyPythonFiles) {
  EXPECT_EQ(backend_->language(), "python");
  ASSERT_EQ(graph_.files.size(), 1u);
  EXPECT_EQ(graph_.files.at(0).path, "src/registry.py");
}

TEST_F(TreeSitterPythonTest, IndexesFunctionsClassesAndMethods) {
  const auto registry = symbols_named("Registry");
  ASSERT_EQ(registry.size(), 1u);
  EXPECT_EQ(registry.at(0)->kind, SymbolKind::Class);
  EXPECT_EQ(registry.at(0)->line, 11u);
  EXPECT_EQ(registry.at(0)->doc, "The registry of things.");
  EXPECT_EQ(registry.at(0)->signature, "class Registry:");

  const auto start = symbols_named("start");
  ASSERT_EQ(start.size(), 1u);
  EXPECT_EQ(start.at(0)->kind, SymbolKind::Method);
  EXPECT_EQ(start.at(0)->qualified, "Registry.start");
  EXPECT_EQ(start.at(0)->line, 14u);
  EXPECT_EQ(start.at(0)->doc, "Start the registry.");
  EXPECT_EQ(start.at(0)->signature, "def start(self, path):");

  const auto stop = symbols_named("stop");
  ASSERT_EQ(stop.size(), 1u);
  EXPECT_EQ(stop.at(0)->kind, SymbolKind::Method);

  const auto make = symbols_named("make_registry");
  ASSERT_EQ(make.size(), 1u);
  EXPECT_EQ(make.at(0)->kind, SymbolKind::Function);
  EXPECT_EQ(make.at(0)->qualified, "make_registry");  // no class scope

  const auto size = symbols_named("DEFAULT_SIZE");
  ASSERT_EQ(size.size(), 1u);
  EXPECT_EQ(size.at(0)->kind, SymbolKind::Variable);
  EXPECT_EQ(size.at(0)->line, 8u);

  // Method bodies are not module scope: self.size is not indexed.
  EXPECT_TRUE(symbols_named("size").empty());
}

TEST_F(TreeSitterPythonTest, RecordsCallsWithCallers) {
  {
    const auto calls = calls_to("open");  // db.open -> attribute name
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.at(0).caller, "start");
    EXPECT_EQ(calls.at(0).line, 16u);
  }
  EXPECT_EQ(calls_to("make_registry").size(), 1u);
  EXPECT_EQ(calls_to("make_registry").at(0).caller, "helper");
  {
    const auto calls = calls_to("Registry");  // Registry() constructor call
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.at(0).caller, "make_registry");
  }
  EXPECT_EQ(calls_to("info").size(), 1u);
  EXPECT_EQ(calls_to("info").at(0).caller, "helper");
}

TEST_F(TreeSitterPythonTest, RecordsImportsAsIncludeEdges) {
  std::vector<std::string> modules;
  for (const auto& include : graph_.includes) {
    modules.push_back(include.included);
    EXPECT_EQ(include.file, "src/registry.py");
    EXPECT_FALSE(include.system);
  }
  std::sort(modules.begin(), modules.end());
  EXPECT_EQ(modules, (std::vector<std::string>{"infra.db", "infra.queue",
                                               "logging", "os"}));
}

TEST_F(TreeSitterPythonTest, UpdateReparsesOnlyTheChangedFiles) {
  write_file(dir_ / "src" / "extra.py", "def extra():\n    return 1\n");
  auto updated = backend_->update(graph_, {"src/extra.py", "README.md"});
  ASSERT_TRUE(updated.has_value()) << updated.error().message;
  ASSERT_EQ(updated->files.size(), 1u);  // README.md is not a Python source
  EXPECT_EQ(updated->files.at(0).path, "src/extra.py");
  EXPECT_EQ(updated->symbols.size(), 1u);
  EXPECT_EQ(updated->symbols.at(0).name, "extra");
}

TEST_F(TreeSitterPythonTest, CheckArchitectureAppliesToImportPaths) {
  // Layering rules match the dotted module path as written.
  cf::codeintel::ArchitectureRules rules;
  rules.layers = {cf::codeintel::LayerRule{"ARCH-1", "src/", "infra"}};
  auto diagnostics = backend_->check_architecture(graph_, rules);
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  EXPECT_EQ(diagnostics->size(), 2u);  // infra.db and infra.queue
}

}  // namespace
