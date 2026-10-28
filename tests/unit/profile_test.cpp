// Unit tests for toolchain profiles (design section 6.7).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/gates/profile.hpp"
#include "cf/plan/plan.hpp"

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
        - [clang-tidy, "-*,bugprone-*,cert-*,cppcoreguidelines-*,performance-*,modernize-*", -p, "{build_dir}", "{changed_files...}"]
        - [cf-tidy, -p, "{build_dir}", "{changed_files...}"]
    format:
      steps:
        - [clang-format, --dry-run, -Werror, "{changed_files...}"]
  c:
    standard_var: CMAKE_C_STANDARD
    guardrails:
      steps:
        - [clang-tidy, "--checks=-*,bugprone-*,cert-*", "--warnings-as-errors=*", -p, "{build_dir}", "{changed_files...}"]
        - [cf-tidy, -p, "{build_dir}", "{changed_files...}"]
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
  coverage:
    tool: llvm-cov
    line: 0.85
    branch: 0.75
    steps:
      - [llvm-cov, "--line={coverage_line}", "--branch={coverage_branch}"]
  mutation:
    tool: mull
    min_score: 0.60
    steps:
      - [mull, "--min-score={mutation_min_score}"]

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
  ASSERT_EQ(profile->languages.at("cpp").guardrails.steps.size(), 2u);
  EXPECT_EQ(profile->languages.at("cpp").guardrails.steps[0].argv[0], "clang-tidy");
  EXPECT_EQ(profile->languages.at("cpp").guardrails.steps[1].argv[0], "cf-tidy");
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
  ASSERT_EQ(profile->coverage.steps.size(), 1u);
  EXPECT_EQ(profile->coverage.steps[0].argv.front(), "llvm-cov");
  ASSERT_EQ(profile->mutation.steps.size(), 1u);
  EXPECT_EQ(profile->mutation.steps[0].argv.front(), "mull");
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
  step.argv = {"cf-tidy", "-p", "{build_dir}", "{changed_files...}"};
  StepContext context;
  context.build_dir = "/build";
  context.changed_files = {"src/a.cpp", "src/b.cpp"};
  auto expanded = cf::gates::expand_step(step, context);
  ASSERT_TRUE(expanded.has_value()) << expanded.error().message;
  const std::vector<std::string> expected = {"cf-tidy", "-p", "/build",
                                             "src/a.cpp", "src/b.cpp"};
  EXPECT_EQ(*expanded, expected);
}

TEST(ProfileTest, ExpandStepSubstitutesRulesDir) {
  GateStep step;
  step.argv = {"semgrep", "--config", "{rules_dir}/semgrep/python.yaml", "--error",
               "{changed_files...}"};
  StepContext context;
  context.rules_dir = "/opt/cf/rules";
  context.changed_files = {"src/a.py"};
  auto expanded = cf::gates::expand_step(step, context);
  ASSERT_TRUE(expanded.has_value()) << expanded.error().message;
  EXPECT_EQ(*expanded,
            (std::vector<std::string>{"semgrep", "--config",
                                      "/opt/cf/rules/semgrep/python.yaml",
                                      "--error", "src/a.py"}));
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

TEST(ProfileTest, ExpandStepSubstitutesStageThreePlaceholders) {
  GateStep step;
  step.argv = {"coverage", "--line={coverage_line}",
               "--branch={coverage_branch}", "{mutation_min_score}"};
  StepContext context;
  context.coverage_line = "85";
  context.coverage_branch = "75";
  context.mutation_min_score = "0.60";
  auto expanded = cf::gates::expand_step(step, context);
  ASSERT_TRUE(expanded.has_value()) << expanded.error().message;
  const std::vector<std::string> expected = {"coverage", "--line=85",
                                             "--branch=75", "0.60"};
  EXPECT_EQ(*expanded, expected);
  EXPECT_EQ(cf::gates::coverage_percent(0.855), "86");
  EXPECT_EQ(cf::gates::ratio_string(0.6), "0.60");
}

TEST(ProfileTest, LoadsTheTypeScriptAndJavaProfiles) {
  auto typescript =
      cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/typescript.yaml");
  ASSERT_TRUE(typescript.has_value()) << typescript.error().message;
  EXPECT_EQ(typescript->id, "typescript");
  EXPECT_NE(typescript->languages.find("typescript"), typescript->languages.end());
  EXPECT_NE(typescript->languages.find("javascript"), typescript->languages.end());
  EXPECT_NE(typescript->build_systems.find("npm"), typescript->build_systems.end());
  EXPECT_NE(typescript->build_systems.find("pnpm"), typescript->build_systems.end());
  EXPECT_NE(typescript->test_frameworks.find("vitest"),
            typescript->test_frameworks.end());
  EXPECT_NE(typescript->test_frameworks.find("jest"),
            typescript->test_frameworks.end());

  auto java = cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/java.yaml");
  ASSERT_TRUE(java.has_value()) << java.error().message;
  EXPECT_EQ(java->id, "java");
  EXPECT_NE(java->languages.find("java"), java->languages.end());
  EXPECT_NE(java->build_systems.find("maven"), java->build_systems.end());
  EXPECT_NE(java->build_systems.find("gradle"), java->build_systems.end());
  EXPECT_NE(java->test_frameworks.find("junit5"),
            java->test_frameworks.end());
  EXPECT_NE(java->test_frameworks.find("junit5-gradle"),
            java->test_frameworks.end());

  auto registry = cf::gates::ProfileRegistry::load_dir(CF_SOURCE_DIR "/profiles");
  ASSERT_TRUE(registry.has_value()) << registry.error().message;
  auto resolved = registry->resolve("typescript", "npm", "vitest");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ((*resolved)->id, "typescript");
  resolved = registry->resolve("javascript", "pnpm", "jest");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ((*resolved)->id, "typescript");
  resolved = registry->resolve("java", "maven", "junit5");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ((*resolved)->id, "java");
  resolved = registry->resolve("java", "gradle", "junit5-gradle");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_EQ((*resolved)->id, "java");

  auto plan = cf::plan::parse_plan_string(
      "plan_id: P-TS\nproduct: Y\nversion: '1'\ntarget:\n  language: typescript\n"
      "  standard: es2022\n  build_system: npm\n  test_framework: vitest\n"
      "milestones:\n  - id: M1\n    title: UI\n    requirements: [R1]\n"
      "    depends_on: []\n    acceptance: ['renders']\n");
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  cf::plan::ValidationOptions options;
  const cf::gates::ProfileRegistry* pointer = &*registry;
  options.profile_exists = [pointer](const cf::plan::TargetSpec& target) {
    return pointer->resolve(target.language, target.build_system,
                            target.test_framework)
        .has_value();
  };
  auto checked = cf::plan::parse_plan_string(
      "plan_id: P-TS\nproduct: Y\nversion: '1'\ntarget:\n  language: typescript\n"
      "  standard: es2022\n  build_system: npm\n  test_framework: vitest\n"
      "milestones:\n  - id: M1\n    title: UI\n    requirements: [R1]\n"
      "    depends_on: []\n    acceptance: ['renders']\n",
      options);
  ASSERT_TRUE(checked.has_value()) << checked.error().message;
}

TEST(ProfileTest, CppProfileRunsMullMutation) {
  auto profile =
      cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/c-cpp.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  ASSERT_EQ(profile->mutation.steps.size(), 1u);
  ASSERT_FALSE(profile->mutation.steps[0].argv.empty());
  EXPECT_EQ(profile->mutation.steps[0].argv[0], "bash");
  EXPECT_EQ(profile->mutation.steps[0].argv[2], "{src_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[3], "{build_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[4], "{mutation_min_score}");
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(CF_SOURCE_DIR) / "rules/scripts/cpp-mutation.sh"));
}

TEST(ProfileTest, PythonProfileRunsMutationWithTheConfiguredScore) {
  auto profile =
      cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/python.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  ASSERT_EQ(profile->mutation.steps.size(), 1u);
  ASSERT_FALSE(profile->mutation.steps[0].argv.empty());
  EXPECT_EQ(profile->mutation.steps[0].argv[0], "bash");
  EXPECT_EQ(profile->mutation.steps[0].argv[2], "{src_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[3], "{build_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[4], "{mutation_min_score}");
}

TEST(ProfileTest, TypeScriptProfileRunsStrykerMutation) {
  auto profile =
      cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/typescript.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  ASSERT_EQ(profile->mutation.steps.size(), 1u);
  ASSERT_FALSE(profile->mutation.steps[0].argv.empty());
  EXPECT_EQ(profile->mutation.steps[0].argv[0], "bash");
  EXPECT_EQ(profile->mutation.steps[0].argv[2], "{src_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[3], "{build_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[4], "{mutation_min_score}");
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(CF_SOURCE_DIR) / "rules/scripts/ts-mutation.sh"));
}

TEST(ProfileTest, JavaProfileRunsPitMutation) {
  auto profile = cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/java.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  ASSERT_EQ(profile->mutation.steps.size(), 1u);
  ASSERT_FALSE(profile->mutation.steps[0].argv.empty());
  EXPECT_EQ(profile->mutation.steps[0].argv[0], "bash");
  EXPECT_EQ(profile->mutation.steps[0].argv[2], "{src_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[3], "{mutation_min_score}");
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(CF_SOURCE_DIR) / "rules/scripts/java-mutation.sh"));
}

TEST(ProfileTest, GoProfileRunsMutationWithTheConfiguredScore) {
  auto profile = cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/go.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  ASSERT_EQ(profile->mutation.steps.size(), 1u);
  ASSERT_FALSE(profile->mutation.steps[0].argv.empty());
  EXPECT_EQ(profile->mutation.steps[0].argv[0], "bash");
  EXPECT_EQ(profile->mutation.steps[0].argv[2], "{src_dir}");
  EXPECT_EQ(profile->mutation.steps[0].argv[3], "{mutation_min_score}");
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(CF_SOURCE_DIR) / "rules/scripts/go-mutation.sh"));
}

TEST(ProfileTest, JavaCoverageUsesJacocoXml) {
  auto profile = cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/java.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  EXPECT_EQ(profile->coverage.tool, "jacoco");
  EXPECT_EQ(profile->coverage.format, "jacoco-xml");
  EXPECT_EQ(profile->coverage.report, "{build_dir}/jacoco.xml");
  ASSERT_EQ(profile->coverage.steps.size(), 1u);
  ASSERT_FALSE(profile->coverage.steps[0].argv.empty());
  EXPECT_EQ(profile->coverage.steps[0].argv[0], "bash");
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(CF_SOURCE_DIR) / "rules/scripts/java-coverage.sh"));
}

TEST(ProfileTest, CppCoverageUsesLlvmCovJson) {
  auto profile =
      cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/c-cpp.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  EXPECT_EQ(profile->coverage.tool, "llvm-cov");
  EXPECT_EQ(profile->coverage.format, "llvm-cov-json");
  EXPECT_EQ(profile->coverage.report, "{build_dir}/coverage/coverage.json");
  ASSERT_EQ(profile->coverage.steps.size(), 1u);
  ASSERT_FALSE(profile->coverage.steps[0].argv.empty());
  EXPECT_EQ(profile->coverage.steps[0].argv[0], "bash");
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(CF_SOURCE_DIR) / "rules/scripts/cpp-coverage.sh"));
}

TEST(ProfileTest, PythonProfileEnforcesLineAndBranchCoverage) {
  auto profile = cf::gates::load_profile_file(CF_SOURCE_DIR "/profiles/python.yaml");
  ASSERT_TRUE(profile.has_value()) << profile.error().message;
  ASSERT_EQ(profile->coverage.steps.size(), 2u);
  EXPECT_EQ(profile->coverage.format, "coverage-json");
  EXPECT_EQ(profile->coverage.report, "{build_dir}/coverage.json");
  StepContext context;
  context.src_dir = "/src";
  context.build_dir = "/build";
  context.coverage_line = "85";
  context.coverage_branch = "75";
  for (const auto& step : profile->coverage.steps) {
    auto expanded = cf::gates::expand_step(step, context);
    ASSERT_TRUE(expanded.has_value()) << expanded.error().message;
    for (const auto& argument : *expanded) {
      EXPECT_EQ(argument.find('{'), std::string::npos) << argument;
    }
  }
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
