// Unit tests for the tree-sitter TypeScript/JavaScript and Java back ends
// (design section 6.5): symbols, methods, imports and call edges.
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

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
                   ("cf_ts_web_test_" + std::to_string(::getpid()) + "_" +
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

bool has_include(const SymbolGraph& graph, const std::string& path) {
  for (const auto& edge : graph.includes) {
    if (edge.included == path) {
      return true;
    }
  }
  return false;
}

const char* kTypeScript = R"ts(
import { open } from "./db";

// Service does work.
export class Service {
  // Start does the work.
  start(path: string): void {
    open(path);
    helper();
  }
}

export interface Opener { open(): void; }
export type Name = string;
export enum Mode { A, B }

export function helper(): void {
  console.log("x");
}

export const arrow = (value: number) => {
  helper();
};
)ts";

const char* kJava = R"java(
package demo;

import java.util.List;

// Service does work.
public class Service {
  private int count;

  // Start does the work.
  public void start(String path) {
    helper();
    new Worker().run();
  }
}

interface Opener { void open(); }
enum Mode { A, B }
record Point(int x, int y) {}

class Helper {
  static void helper() {}
}
)java";

class TreeSitterWebTest : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = make_temp_dir(); }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  SymbolGraph index(const std::unique_ptr<cf::codeintel::LanguageBackend>& backend,
                    const std::string& name, const std::string& content) {
    write_file(dir_ / name, content);
    auto graph = backend->index(dir_);
    EXPECT_TRUE(graph.has_value()) << graph.error().message;
    return graph.has_value() ? std::move(*graph) : SymbolGraph{};
  }

  std::filesystem::path dir_;
};

TEST_F(TreeSitterWebTest, TypeScriptIndexesClassesMethodsFunctionsAndCalls) {
  auto backend = cf::codeintel::make_typescript_backend();
  const auto graph = index(backend, "service.ts", kTypeScript);
  EXPECT_EQ(backend->language(), "typescript");
  ASSERT_EQ(graph.files.size(), 1u);

  const Symbol* service = find_symbol(graph, "Service");
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(service->kind, SymbolKind::Class);
  EXPECT_EQ(service->qualified, "Service");

  const Symbol* start = find_symbol(graph, "start");
  ASSERT_NE(start, nullptr);
  EXPECT_EQ(start->kind, SymbolKind::Method);
  EXPECT_EQ(start->qualified, "Service.start");
  EXPECT_EQ(start->doc, "Start does the work.");

  const Symbol* helper = find_symbol(graph, "helper");
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->kind, SymbolKind::Function);
  const Symbol* arrow = find_symbol(graph, "arrow");
  ASSERT_NE(arrow, nullptr);
  EXPECT_EQ(arrow->kind, SymbolKind::Function);

  const Symbol* opener = find_symbol(graph, "Opener");
  ASSERT_NE(opener, nullptr);
  EXPECT_EQ(opener->kind, SymbolKind::Interface);
  const Symbol* mode = find_symbol(graph, "Mode");
  ASSERT_NE(mode, nullptr);
  EXPECT_EQ(mode->kind, SymbolKind::Enum);
  const Symbol* name = find_symbol(graph, "Name");
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->kind, SymbolKind::Typedef);

  EXPECT_TRUE(has_include(graph, "./db"));
  EXPECT_TRUE(has_call(graph, "Service.start", "open"));
  EXPECT_TRUE(has_call(graph, "Service.start", "helper"));
  EXPECT_TRUE(has_call(graph, "helper", "log"));
  EXPECT_TRUE(has_call(graph, "arrow", "helper"));
}

TEST_F(TreeSitterWebTest, JavaScriptBackendIndexesJsFiles) {
  auto backend = cf::codeintel::make_javascript_backend();
  const auto graph = index(backend, "service.js",
                           "import { open } from './db';\n"
                           "export function run(path) { open(path); }\n");
  EXPECT_EQ(backend->language(), "javascript");
  const Symbol* run = find_symbol(graph, "run");
  ASSERT_NE(run, nullptr);
  EXPECT_EQ(run->kind, SymbolKind::Function);
  EXPECT_TRUE(has_call(graph, "run", "open"));
}

TEST_F(TreeSitterWebTest, JavaIndexesTypesMethodsFieldsAndCalls) {
  auto backend = cf::codeintel::make_java_backend();
  const auto graph = index(backend, "Service.java", kJava);
  EXPECT_EQ(backend->language(), "java");
  ASSERT_EQ(graph.files.size(), 1u);

  const Symbol* service = find_symbol(graph, "Service");
  ASSERT_NE(service, nullptr);
  EXPECT_EQ(service->kind, SymbolKind::Class);
  const Symbol* start = find_symbol(graph, "start");
  ASSERT_NE(start, nullptr);
  EXPECT_EQ(start->kind, SymbolKind::Method);
  EXPECT_EQ(start->qualified, "Service.start");
  EXPECT_EQ(start->doc, "Start does the work.");
  const Symbol* count = find_symbol(graph, "count");
  ASSERT_NE(count, nullptr);
  EXPECT_EQ(count->kind, SymbolKind::Variable);
  EXPECT_EQ(count->qualified, "Service.count");

  const Symbol* opener = find_symbol(graph, "Opener");
  ASSERT_NE(opener, nullptr);
  EXPECT_EQ(opener->kind, SymbolKind::Interface);
  const Symbol* mode = find_symbol(graph, "Mode");
  ASSERT_NE(mode, nullptr);
  EXPECT_EQ(mode->kind, SymbolKind::Enum);
  const Symbol* point = find_symbol(graph, "Point");
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->kind, SymbolKind::Struct);
  const Symbol* helper = find_symbol(graph, "helper");
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->qualified, "Helper.helper");

  EXPECT_TRUE(has_include(graph, "java.util.List"));
  EXPECT_TRUE(has_call(graph, "Service.start", "helper"));
  EXPECT_TRUE(has_call(graph, "Service.start", "run"));
  EXPECT_TRUE(has_call(graph, "Service.start", "Worker"));
}

}  // namespace
