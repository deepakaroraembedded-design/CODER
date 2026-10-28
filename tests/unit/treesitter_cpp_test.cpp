// Unit tests for the tree-sitter C++ fallback back end (design section 6.5).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/codeintel/treesitter_backend.hpp"

namespace {

using cf::codeintel::Symbol;
using cf::codeintel::SymbolGraph;
using cf::codeintel::SymbolKind;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_ts_cpp_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

const Symbol* find_symbol(const SymbolGraph& graph, const std::string& name) {
  for (const auto& symbol : graph.symbols) {
    if (symbol.name == name) {
      return &symbol;
    }
  }
  return nullptr;
}

bool has_call(const SymbolGraph& graph, const std::string& caller,
              const std::string& callee) {
  for (const auto& edge : graph.calls) {
    if (edge.caller == caller && edge.callee == callee) {
      return true;
    }
  }
  return false;
}

bool has_include(const SymbolGraph& graph, const std::string& path, bool system) {
  for (const auto& edge : graph.includes) {
    if (edge.included == path && edge.system == system) {
      return true;
    }
  }
  return false;
}

const char* kSource = R"cpp(
#include <vector>
#include "local.hpp"

namespace demo {

struct Point { int x; int y; };

// Widget does work.
class Widget {
 public:
  // Run does the work.
  void run(int n) { helper(n); }
  int value;
};

// Helper returns its input.
int helper(int n) { return n; }

}
)cpp";

TEST(TreeSitterCppTest, IndexesNamespacesClassesMethodsAndCalls) {
  const auto dir = make_temp_dir();
  write_file(dir / "widget.cpp", kSource);
  write_file(dir / "README.md", "# not code\n");
  auto backend = cf::codeintel::make_cpp_backend();
  auto graph = backend->index(dir);
  ASSERT_TRUE(graph.has_value()) << graph.error().message;
  EXPECT_EQ(backend->language(), "cpp");
  ASSERT_EQ(graph->files.size(), 1u);

  const Symbol* ns = find_symbol(*graph, "demo");
  ASSERT_NE(ns, nullptr);
  EXPECT_EQ(ns->kind, SymbolKind::Namespace);
  const Symbol* point = find_symbol(*graph, "Point");
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->kind, SymbolKind::Struct);
  EXPECT_EQ(point->qualified, "demo::Point");
  const Symbol* widget = find_symbol(*graph, "Widget");
  ASSERT_NE(widget, nullptr);
  EXPECT_EQ(widget->kind, SymbolKind::Class);
  EXPECT_EQ(widget->qualified, "demo::Widget");
  const Symbol* run = find_symbol(*graph, "run");
  ASSERT_NE(run, nullptr);
  EXPECT_EQ(run->kind, SymbolKind::Method);
  EXPECT_EQ(run->qualified, "demo::Widget::run");
  EXPECT_EQ(run->doc, "Run does the work.");
  const Symbol* helper = find_symbol(*graph, "helper");
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->kind, SymbolKind::Function);
  EXPECT_EQ(helper->qualified, "demo::helper");
  EXPECT_EQ(helper->doc, "Helper returns its input.");

  EXPECT_TRUE(has_include(*graph, "vector", true));
  EXPECT_TRUE(has_include(*graph, "local.hpp", false));
  EXPECT_TRUE(has_call(*graph, "demo::Widget::run", "helper"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
