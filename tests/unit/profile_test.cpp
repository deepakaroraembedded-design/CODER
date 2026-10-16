// Unit tests for toolchain profiles (design section 6.7).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/gates/profile.hpp"

namespace {

using cf::gates::GateStep;
using cf::gates::Profile;
using cf::gates::ProfileRegistry;
using cf::gates::StepContext;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_profile_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

const char* kExampleProfile = R"yaml(
id: c-cpp
image: cf-toolchain-c-cpp:2026.10
codeintel_backend: clang

languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails:
      steps:
        - [clang-tidy, --load=libcf-swt-checks.so, -p, "{build_dir}", "{changed_files...}"]
    format:
      steps:
        - [clang-format, --dry-run, -Werror, "{changed_files...}"]
  c:
    standard_var: CMAKE_C_STANDARD
    guardrails:
      steps:
        - [clang-tidy, --load=libcf-swt-checks.so, "--checks=-*,bugprone-*,cert-*,cf-swt-c-*", -p, "{build_dir}", "{changed_files...}"]
    format:
      steps:
        - [clang-format, --dry-run, -Werror, "{changed_files...}"]

build_systems:
  cmake:
    steps:
      - [cmake, -S, "{src_dir}", -B, "{build_dir}", "-D{standard_var}={standard_number}", -DCMAKE_EXPORT_COMPILE_COMMANDS=ON]
      - [cmake, --build, "{build_dir}"]
  make:
    steps:
      - [make, -C, "{src_dir}", "O={build_dir}", "CFLAGS={standard_flag} -Wall -Wextra -Werror"]
    agent_commands: [make]

test_frameworks:
  gtest:  { steps: [[ctest, --test-dir, "{build_dir}", --output-on-failure]], report: junit, languages: [cpp] }
  catch2: { steps: [[ctest, --test-dir, "{build_dir}", --output-on-failure]], report: junit, languages: [cpp] }
  unity:  { steps: [["{build_dir}/run_tests", --junit]], report: junit, languages: [c] }

common:
  analysis:
    steps:
      - [cppcheck, --enable=warning,performance, --error-exitcode=1, "--project={build_dir}/compile_commands.json"]
  runtime:  { presets: [asan-ubsan], tsan_when: "component.concurrent" }
  coverage: { tool: llvm-cov, line: 0.85, branch: 0.75 }
  mutation: { tool: mull, min_score: 0.60 }

exec_from_source: []
)yaml";

TEST(ProfileTest, ParsesTheExampleProfile) {
  auto profile = cf::gates::load_profile_string(kExampleProfile);
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  EXPECT_EQ(profile->id, "c-cpp");
  EXPECT_EQ(profile->image, "cf-toolchain-c-cpp:2026.10");
  EXPECT_EQ(profile->codeintel_backend, "clang");
  ASSERT_EQ(profile->languages.size(), 2u);
  EXPECT_EQ(profile->languages.at("cpp").standard_var, "CMAKE_CXX_STANDARD");
  ASSERT_EQ(profile->languages.at("cpp").guardrails.steps.size(), 1u);
  EXPECT_EQ(profile->languages.at("cpp").guardrails.steps[0].argv[0], "clang-tidy");
  ASSERT_EQ(profile->build_systems.count("cmake"), 1u);
  EXPECT_EQ(profile->build_systems.at("cmake").steps.size(), 2u);
  EXPECT_EQ(profile->build_systems.at("make").agent_commands,
            std::vector<std::string>{"make"});
  ASSERT_EQ(profile->test_frameworks.count("gtest"), 1u);
  EXPECT_EQ(profile->test_frameworks.at("gtest").report, "junit");
  EXPECT_EQ(profile->test_frameworks.at("gtest").languages,
            std::vector<std::string>{"cpp"});
  ASSERT_EQ(profile->common.count("analysis"), 1u);
  EXPECT_EQ(profile->common.at("analysis").steps.size(), 1u);
  EXPECT_EQ(profile->runtime.presets, std::vector<std::string>{"asan-ubsan"});
  EXPECT_EQ(profile->runtime.tsan_when, "component.concurrent");
  EXPECT_EQ(profile->coverage.tool, "llvm-cov");
  EXPECT_DOUBLE_EQ(profile->coverage.line, 0.85);
  EXPECT_DOUBLE_EQ(profile->coverage.branch, 0.75);
  EXPECT_EQ(profile->mutation.tool, "mull");
  EXPECT_DOUBLE_EQ(profile->mutation.min_score, 0.60);
  EXPECT_TRUE(profile->exec_from_source.empty());
}

TEST(ProfileTest, MissingIdFails) {
  auto profile = cf::gates::load_profile_string(
      "image: img\nlanguages:\n  cpp:\n    standard_var: V\n");
  ASSERT_FALSE(profile.has_value());
  EXPECT_EQ(profile.error().code, cf::gates::error_codes::kProfileSchemaInvalid);
}

TEST(ProfileTest, NoLanguagesFails) {
  auto profile = cf::gates::load_profile_string("id: p\nimage: img\nlanguages: {}\n");
  ASSERT_FALSE(profile.has_value());
  EXPECT_EQ(profile.error().code, cf::gates::error_codes::kProfileSchemaInvalid);
}

TEST(ProfileTest, EmptyStepArgumentFails) {
  auto profile = cf::gates::load_profile_string(
      "id: p\nimage: img\nlanguages:\n  cpp:\n    standard_var: V\n"
      "    format:\n      steps:\n        - [clang-format, \"\"]\n");
  ASSERT_FALSE(profile.has_value());
  EXPECT_EQ(profile.error().code, cf::gates::error_codes::kProfileSchemaInvalid);
}

TEST(ProfileTest, WildcardExecFromSourceFails) {
  auto profile = cf::gates::load_profile_string(
      "id: p\nimage: img\nlanguages:\n  cpp:\n    standard_var: V\n"
      "exec_from_source: [\"scripts/*.sh\"]\n");
  ASSERT_FALSE(profile.has_value());
  EXPECT_EQ(profile.error().code, cf::gates::error_codes::kProfileSchemaInvalid);
}

TEST(ProfileTest, MalformedYamlFails) {
  auto profile = cf::gates::load_profile_string("id: [unclosed\n");
  ASSERT_FALSE(profile.has_value());
  EXPECT_EQ(profile.error().code, cf::gates::error_codes::kProfileParseFailed);
}

TEST(ProfileTest, LoadsProfileFile) {
  const auto dir = make_temp_dir();
  const auto path = dir / "c-cpp.yaml";
  {
    std::ofstream out(path);
    out << kExampleProfile;
  }
  auto profile = cf::gates::load_profile_file(path);
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  EXPECT_EQ(profile->id, "c-cpp");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(ProfileTest, RegistryRejectsDuplicateId) {
  auto first = cf::gates::load_profile_string(kExampleProfile);
  ASSERT_TRUE(first.has_value());
  ProfileRegistry registry;
  ASSERT_TRUE(registry.add(std::move(*first)).has_value());
  auto second = cf::gates::load_profile_string(kExampleProfile);
  ASSERT_TRUE(second.has_value());
  const auto added = registry.add(std::move(*second));
  ASSERT_FALSE(added.has_value());
  EXPECT_EQ(added.error().code, cf::gates::error_codes::kProfileDuplicate);
}

TEST(ProfileTest, RegistryResolvesCppCmakeGtest) {
  ProfileRegistry registry;
  auto profile = cf::gates::load_profile_string(kExampleProfile);
  ASSERT_TRUE(profile.has_value());
  ASSERT_TRUE(registry.add(std::move(*profile)).has_value());

  auto resolved = registry.resolve("cpp", "cmake", "gtest");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ((*resolved)->id, "c-cpp");

  auto unity = registry.resolve("c", "cmake", "unity");
  ASSERT_TRUE(unity.has_value()) << unity.error().message;
  EXPECT_EQ((*unity)->id, "c-cpp");
}

TEST(ProfileTest, RegistryRejectsFrameworkForOtherLanguage) {
  ProfileRegistry registry;
  auto profile = cf::gates::load_profile_string(kExampleProfile);
  ASSERT_TRUE(profile.has_value());
  ASSERT_TRUE(registry.add(std::move(*profile)).has_value());

  // gtest lists languages: [cpp], so a C target cannot use it.
  auto resolved = registry.resolve("c", "cmake", "gtest");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_EQ(resolved.error().code, cf::gates::error_codes::kNoMatchingToolchain);
}

TEST(ProfileTest, RegistryRejectsUnknownCombinations) {
  ProfileRegistry registry;
  auto profile = cf::gates::load_profile_string(kExampleProfile);
  ASSERT_TRUE(profile.has_value());
  ASSERT_TRUE(registry.add(std::move(*profile)).has_value());

  EXPECT_FALSE(registry.resolve("python", "cmake", "gtest").has_value());
  EXPECT_FALSE(registry.resolve("cpp", "bazel", "gtest").has_value());
  EXPECT_FALSE(registry.resolve("cpp", "cmake", "vitest").has_value());
}

TEST(ProfileTest, LoadDirLoadsAllProfilesAndRejectsDuplicates) {
  const auto dir = make_temp_dir();
  {
    std::ofstream out(dir / "a.yaml");
    out << kExampleProfile;
  }
  {
    std::ofstream out(dir / "b.yaml");
    out << "id: python\nimage: img\nlanguages:\n  python:\n    standard_var: PY\n";
  }
  auto registry = ProfileRegistry::load_dir(dir);
  ASSERT_TRUE(registry.has_value()) << registry.error().message;
  EXPECT_EQ(registry->all().size(), 2u);

  {
    std::ofstream out(dir / "c.yaml");
    out << kExampleProfile;  // duplicate id c-cpp
  }
  auto duplicate = ProfileRegistry::load_dir(dir);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code, cf::gates::error_codes::kProfileDuplicate);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(ProfileTest, MissingDirFails) {
  auto registry = ProfileRegistry::load_dir("/nonexistent/profiles");
  ASSERT_FALSE(registry.has_value());
  EXPECT_EQ(registry.error().code, cf::gates::error_codes::kProfileNotFound);
}

TEST(ProfileTest, ExpandStepSplicesChangedFiles) {
  GateStep step;
  step.argv = {"clang-tidy", "--load=libcf-swt-checks.so", "-p", "{build_dir}",
               "{changed_files...}"};
  StepContext context;
  context.build_dir = "/build";
  context.changed_files = {"src/a.cpp", "src/b.cpp"};
  auto expanded = cf::gates::expand_step(step, context);
  ASSERT_TRUE(expanded.has_value()) << expanded.error().message;
  const std::vector<std::string> expected = {"clang-tidy", "--load=libcf-swt-checks.so",
                                             "-p", "/build", "src/a.cpp", "src/b.cpp"};
  EXPECT_EQ(*expanded, expected);
}

TEST(ProfileTest, ExpandStepSubstitutesStandardPlaceholders) {
  GateStep step;
  step.argv = {"cmake", "-S", "{src_dir}", "-B", "{build_dir}",
               "-D{standard_var}={standard_number}"};
  StepContext context;
  context.src_dir = "/src";
  context.build_dir = "/build";
  context.standard = "c++20";
  context.standard_number = "20";
  context.standard_var = "CMAKE_CXX_STANDARD";
  auto expanded = cf::gates::expand_step(step, context);
  ASSERT_TRUE(expanded.has_value()) << expanded.error().message;
  const std::vector<std::string> expected = {"cmake", "-S", "/src", "-B", "/build",
                                             "-DCMAKE_CXX_STANDARD=20"};
  EXPECT_EQ(*expanded, expected);
}

TEST(ProfileTest, ExpandStepRejectsUnknownPlaceholder) {
  GateStep step;
  step.argv = {"tool", "{unknown_placeholder}"};
  auto expanded = cf::gates::expand_step(step, StepContext{});
  ASSERT_FALSE(expanded.has_value());
  EXPECT_EQ(expanded.error().code, cf::gates::error_codes::kUnknownPlaceholder);
}

TEST(ProfileTest, StandardNumberDerivation) {
  EXPECT_EQ(cf::gates::standard_number("c++20"), "20");
  EXPECT_EQ(cf::gates::standard_number("c++17"), "17");
  EXPECT_EQ(cf::gates::standard_number("c11"), "11");
  EXPECT_EQ(cf::gates::standard_number("3.12"), "12");
  EXPECT_EQ(cf::gates::standard_number("go1.22"), "22");
  EXPECT_EQ(cf::gates::standard_number("latest"), "");
}

}  // namespace
