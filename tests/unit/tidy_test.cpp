// Unit tests for the cf-swt company guardrail checks (design section 6.5).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <unistd.h>

#include <yaml-cpp/yaml.h>

#include "cf/tidy/checks.hpp"

namespace {

namespace fs = std::filesystem;

fs::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = fs::temp_directory_path() /
                   ("cf_tidy_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

void write_file(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

void write_compile_commands(const fs::path& dir, const std::string& file,
                            const std::string& command = "c++ -std=c++17") {
  std::ofstream out(dir / "compile_commands.json", std::ios::binary);
  out << R"([{"directory":")" << dir.generic_string() << R"(","file":")"
      << file << R"(","command":")" << command << " -c " << file << R"("}])";
}

TEST(TidyTest, FlagsRawNewButNotPlacementOrExemptNamespace) {
  const auto dir = make_temp_dir();
  const std::string source = R"cpp(
struct Foo {};
void* operator new(unsigned long, void*);
void* malloc(unsigned long);
void free(void*);
void bad() { Foo* p = new Foo(); delete p; }
void* raw_alloc() { void* p = malloc(4); free(p); return p; }
void placement(void* b) { new (b) Foo(); }
namespace exempt { void ok() { Foo* p = new Foo(); static_cast<void>(p); } }
)cpp";
  write_file(dir / "test.cpp", source);
  write_compile_commands(dir, "test.cpp");

  cf::tidy::TidyOptions options;
  options.allowed_namespaces = "::exempt";
  auto diagnostics =
      cf::tidy::run_company_checks(dir, {dir / "test.cpp"}, options);
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 4u);
  const auto find = [&diagnostics](const std::string& check)
      -> const cf::tidy::TidyDiagnostic* {
    for (const auto& diagnostic : *diagnostics) {
      if (diagnostic.check == check) {
        return &diagnostic;
      }
    }
    return nullptr;
  };
  const auto* raw_new = find("cf-swt-no-raw-new");
  ASSERT_NE(raw_new, nullptr);
  EXPECT_EQ(raw_new->line, 6u);
  EXPECT_NE(raw_new->message.find("CG-4.2"), std::string::npos);
  const auto* raw_delete = find("cf-swt-no-raw-delete");
  ASSERT_NE(raw_delete, nullptr);
  EXPECT_EQ(raw_delete->line, 6u);
  const auto* raw_malloc = find("cf-swt-no-raw-malloc");
  ASSERT_NE(raw_malloc, nullptr);
  EXPECT_EQ(raw_malloc->line, 7u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, ReadsAllowedNamespacesFromClangTidyConfig) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "struct Foo {};\n"
             "namespace exempt { void ok() { Foo* p = new Foo(); static_cast<void>(p); } }\n");
  write_file(dir / ".clang-tidy",
             "CheckOptions:\n"
             "  - key: cf-swt-no-raw-new.AllowedNamespaces\n"
             "    value: \"::exempt\"\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  EXPECT_TRUE(diagnostics->empty());

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, ListsImplementedChecksAndCatalog) {
  const auto checks = cf::tidy::implemented_checks();
  ASSERT_EQ(checks.size(), 18u);
  const YAML::Node catalog =
      YAML::LoadFile(std::string(CF_SOURCE_DIR) + "/rules/guardrails.yaml");
  std::set<std::string> catalog_checks;
  for (const auto& rule : catalog["rules"]) {
    const std::string status = rule["status"].as<std::string>();
    EXPECT_TRUE(status == "automated" || status == "reviewer-only");
    if (rule["checks"]) {
      for (const auto& check : rule["checks"]) {
        catalog_checks.insert(check.as<std::string>());
      }
    }
  }
  for (const auto& check : checks) {
    EXPECT_EQ(catalog_checks.count(check.id), 1u) << check.id;
  }
}

TEST(TidyTest, FlagsUsingNamespaceInHeadersAndGoto) {
  const auto dir = make_temp_dir();
  write_file(dir / "header.hpp",
             "namespace inner {}\n"
             "using namespace inner;\n");
  write_file(dir / "test.cpp",
             "#include \"header.hpp\"\n"
             "void jump() { goto end; end: return; }\n");
  write_compile_commands(dir, "test.cpp", "c++ -std=c++17 -I.");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  std::set<std::string> checks;
  for (const auto& diagnostic : *diagnostics) {
    checks.insert(diagnostic.check);
  }
  EXPECT_EQ(checks.count("cf-swt-no-using-namespace-in-header"), 1u);
  EXPECT_EQ(checks.count("cf-swt-no-goto"), 1u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsWeakHashFunctions) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "void MD5();\n"
             "void use_hash() { MD5(); }\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-weak-hash");

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsPublicStaticDataInClasses) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "class Counter { public: static int total; };\n"
             "struct Plain { static int total; };\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  std::size_t public_static = 0;
  for (const auto& diagnostic : *diagnostics) {
    if (diagnostic.check == "cf-swt-no-public-static-data") {
      ++public_static;
    }
  }
  EXPECT_EQ(public_static, 1u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsSleepsInTests) {
  const auto dir = make_temp_dir();
  write_file(dir / "tests" / "test.cpp",
             "void sleep(unsigned int);\n"
             "void wait() { sleep(1); }\n");
  write_compile_commands(dir / "tests", "test.cpp");

  auto diagnostics =
      cf::tidy::run_company_checks(dir / "tests", {dir / "tests" / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-sleep-in-test");

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsPublicDataMembersInClassesOnly) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "class Widget { public: int value; };\n"
             "struct Plain { int value; };\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-public-data-member");
  EXPECT_EQ((*diagnostics)[0].line, 1u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, RunsOnlySelectedChecks) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "void jump() { goto end; end: return; }\n"
             "void raw() { int* value = new int(1); delete value; }\n");
  write_compile_commands(dir, "test.cpp");

  cf::tidy::TidyOptions options;
  options.only_checks = {"cf-swt-no-goto"};
  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"}, options);
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-goto");

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, DisablesIndividualChecks) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "void jump() { goto end; end: return; }\n");
  write_compile_commands(dir, "test.cpp");

  cf::tidy::TidyOptions options;
  options.disabled_checks = {"cf-swt-no-goto"};
  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"}, options);
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  EXPECT_TRUE(diagnostics->empty());

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsMultipleInheritance) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "struct A {};\n"
             "struct B {};\n"
             "struct C : A, B {};\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-multiple-inheritance");
  EXPECT_EQ((*diagnostics)[0].line, 3u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsUnboundedCStringFunctions) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "char* strcpy(char* destination, const char* source);\n"
             "void copy(char* destination, const char* source) {\n"
             "  strcpy(destination, source);\n"
             "}\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-unsafe-c-function");
  EXPECT_EQ((*diagnostics)[0].line, 3u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsRawArrays) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "struct Buffer { int values[4]; };\n"
             "void make_buffer() { int values[4] = {0}; static_cast<void>(values); }\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  std::size_t raw_arrays = 0;
  for (const auto& diagnostic : *diagnostics) {
    if (diagnostic.check == "cf-swt-no-raw-array") {
      ++raw_arrays;
    }
  }
  EXPECT_EQ(raw_arrays, 2u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsUnsafeCasts) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "void reinterpret(int* value) {\n"
             "  auto* raw = reinterpret_cast<void*>(value);\n"
             "  static_cast<void>(raw);\n"
             "}\n"
             "void remove_const(const int* value) {\n"
             "  auto* mutable_value = const_cast<int*>(value);\n"
             "  static_cast<void>(mutable_value);\n"
             "}\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  std::set<std::string> checks;
  for (const auto& diagnostic : *diagnostics) {
    checks.insert(diagnostic.check);
  }
  EXPECT_EQ(checks.count("cf-swt-no-reinterpret-cast"), 1u);
  EXPECT_EQ(checks.count("cf-swt-no-const-cast"), 1u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsCStyleCasts) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "void convert(double value) {\n"
             "  int rounded = (int)value;\n"
             "  static_cast<void>(rounded);\n"
             "}\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-c-style-cast");
  EXPECT_EQ((*diagnostics)[0].line, 2u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsBooleanParameters) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "void configure(bool enabled);\n"
             "void configure_twice(int enabled);\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-bool-param");
  EXPECT_EQ((*diagnostics)[0].line, 1u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsDetachedThreads) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "struct Thread { void detach(); };\n"
             "void spawn(Thread& thread) { thread.detach(); }\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-detached-thread");
  EXPECT_EQ((*diagnostics)[0].line, 2u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsHardcodedCredentials) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp",
             "const char* password = \"hunter2\";\n"
             "const char* public_name = \"hunter2\";\n"
             "const char* api_key = nullptr;\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 1u);
  EXPECT_EQ((*diagnostics)[0].check, "cf-swt-no-hardcoded-secret");
  EXPECT_EQ((*diagnostics)[0].line, 1u);

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, AllowsCAllocationInCTranslationUnits) {
  // The raw-allocation rule is a C++ ownership rule, not a C restriction.
  const auto dir = make_temp_dir();
  write_file(dir / "test.c",
             "void* malloc(unsigned long);\n"
             "void free(void*);\n"
             "void* raw(void) { void* p = malloc(4); free(p); return p; }\n");
  write_compile_commands(dir, "test.c", "cc -std=c11");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.c"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  EXPECT_TRUE(diagnostics->empty());

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, FlagsProjectMacroAtItsUseSite) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp", R"cpp(#define MAKE_FOO() (new Foo())
struct Foo {};
void use_macro() { Foo* p = MAKE_FOO(); delete p; }
)cpp");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  ASSERT_EQ(diagnostics->size(), 2u);
  for (const auto& diagnostic : *diagnostics) {
    EXPECT_EQ(diagnostic.line, 3u);
    EXPECT_EQ(diagnostic.file, "test.cpp");
  }

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(TidyTest, AcceptsCleanSources) {
  const auto dir = make_temp_dir();
  write_file(dir / "test.cpp", "struct Foo {};\nvoid use(Foo* p) { static_cast<void>(p); }\n");
  write_compile_commands(dir, "test.cpp");

  auto diagnostics = cf::tidy::run_company_checks(dir, {dir / "test.cpp"});
  ASSERT_TRUE(diagnostics.has_value()) << diagnostics.error().message;
  EXPECT_TRUE(diagnostics->empty());

  std::error_code error;
  fs::remove_all(dir, error);
}

}  // namespace
