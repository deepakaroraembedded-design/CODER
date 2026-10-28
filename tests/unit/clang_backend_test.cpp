// Unit tests for the Clang LibTooling C/C++ back end (design section 6.5).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/codeintel/clang_backend.hpp"

namespace {

using cf::codeintel::Symbol;
using cf::codeintel::SymbolGraph;
using cf::codeintel::SymbolKind;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_clang_test_" + std::to_string(::getpid()) + "_" +
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

int helper(int n);

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

TEST(ClangBackendTest, IndexesSymbolsCallsAndIncludes) {
  const auto dir = make_temp_dir();
  write_file(dir / "widget.cpp", kSource);
  write_file(dir / "README.md", "# not code\n");
  auto backend = cf::codeintel::make_clang_backend(
      "cpp", {".cc", ".cpp", ".cxx", ".c++", ".cp"});
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
  EXPECT_NE(run->doc.find("Run does the work."), std::string::npos);
  const Symbol* helper = find_symbol(*graph, "helper");
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->kind, SymbolKind::Function);
  EXPECT_EQ(helper->qualified, "demo::helper");
  EXPECT_NE(helper->doc.find("Helper returns its input."), std::string::npos);

  EXPECT_LT(graph->symbols.size(), 20u);
  for (const auto& symbol : graph->symbols) {
    EXPECT_EQ(symbol.file.rfind("..", 0), std::string::npos)
        << symbol.qualified << " points outside the indexed root";
  }
  EXPECT_TRUE(has_include(*graph, "vector", true));
  EXPECT_TRUE(has_include(*graph, "local.hpp", false));
  EXPECT_TRUE(has_call(*graph, "demo::Widget::run", "helper"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(ClangBackendTest, UpdatesOnlyChangedTranslationUnit) {
  const auto dir = make_temp_dir();
  write_file(dir / "alpha.cpp", "int alpha() { return 1; }\n");
  write_file(dir / "beta.cpp", "int beta() { return 2; }\n");
  auto backend = cf::codeintel::make_clang_backend("cpp", {".cpp"});
  auto graph = backend->index(dir);
  ASSERT_TRUE(graph.has_value()) << graph.error().message;
  ASSERT_NE(find_symbol(*graph, "alpha"), nullptr);
  ASSERT_NE(find_symbol(*graph, "beta"), nullptr);

  write_file(dir / "alpha.cpp", "int gamma() { return 3; }\n");
  auto updated = backend->update(*graph, {std::filesystem::path("alpha.cpp")});
  ASSERT_TRUE(updated.has_value()) << updated.error().message;
  EXPECT_NE(find_symbol(*updated, "gamma"), nullptr);
  EXPECT_EQ(find_symbol(*updated, "alpha"), nullptr);
  EXPECT_EQ(find_symbol(*updated, "beta"), nullptr);
  EXPECT_EQ(updated->files.size(), 1u);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(ClangBackendTest, IndexesCTranslationUnits) {
  const auto dir = make_temp_dir();
  write_file(dir / "math.c", "int add(int a, int b) { return a + b; }\n");
  auto backend = cf::codeintel::make_clang_backend("c", {".c"});
  auto graph = backend->index(dir);
  ASSERT_TRUE(graph.has_value()) << graph.error().message;
  const Symbol* add = find_symbol(*graph, "add");
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->kind, SymbolKind::Function);
  EXPECT_EQ(add->qualified, "add");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
