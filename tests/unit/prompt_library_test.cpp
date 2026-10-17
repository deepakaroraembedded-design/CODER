// Unit tests for the versioned prompt templates (design section 6.6).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <unistd.h>

#include "cf/orch/prompt_library.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_prompt_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

TEST(PromptLibraryTest, EmbeddedDefaultsRender) {
  const cf::orch::PromptLibrary library = cf::orch::PromptLibrary::defaults();
  EXPECT_TRUE(library.has("coder"));
  EXPECT_TRUE(library.has("reviewer"));
  EXPECT_TRUE(library.has("qa"));

  std::map<std::string, std::string, std::less<>> values;
  values["run_id"] = "R12";
  values["milestone_id"] = "M3";
  values["language"] = "cpp";
  values["standard"] = "c++20";
  values["requirements"] = "REQ-010";
  values["brief"] = "Implement the registry";
  values["submission_tools"] = "submit_work";
  values["commands"] = "cmake, ninja, ctest";
  auto rendered = library.render("coder", values);
  ASSERT_TRUE(rendered.has_value()) << rendered.error().message;
  EXPECT_NE(rendered->find("Coder agent"), std::string::npos);
  EXPECT_NE(rendered->find("Run: R12"), std::string::npos);
  EXPECT_NE(rendered->find("cpp (c++20)"), std::string::npos);
  EXPECT_NE(rendered->find("REQ-010"), std::string::npos);
  EXPECT_NE(rendered->find("Implement the registry"), std::string::npos);
  EXPECT_NE(rendered->find("submit_work"), std::string::npos);
  EXPECT_NE(rendered->find("Do not ask questions"), std::string::npos);
  EXPECT_NE(rendered->find("cmake, ninja, ctest"), std::string::npos);
}

TEST(PromptLibraryTest, EmptyValuesDropTheirLines) {
  const cf::orch::PromptLibrary library = cf::orch::PromptLibrary::defaults();
  std::map<std::string, std::string, std::less<>> values;
  values["brief"] = "Do the task";
  values["submission_tools"] = "submit_work";
  auto rendered = library.render("coder", values);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_EQ(rendered->find("Requirements:"), std::string::npos);
  EXPECT_EQ(rendered->find("Open defects:"), std::string::npos);
}

TEST(PromptLibraryTest, LoadDirOverridesRoles) {
  const auto dir = make_temp_dir();
  write_file(dir / "coder.md",
             "Custom coder prompt for {{language}}.\n\nTask: {{brief}}\n");
  auto library = cf::orch::PromptLibrary::load_dir(dir);
  ASSERT_TRUE(library.has_value()) << library.error().message;
  std::map<std::string, std::string, std::less<>> values;
  values["language"] = "rust";
  values["brief"] = "write it";
  auto rendered = library->render("coder", values);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_NE(rendered->find("Custom coder prompt for rust."), std::string::npos);
  EXPECT_TRUE(library->has("reviewer"));  // defaults survive

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(PromptLibraryTest, UnknownPlaceholderIsRejected) {
  const auto dir = make_temp_dir();
  write_file(dir / "coder.md", "Prompt with {{nonsense}}.\n");
  auto library = cf::orch::PromptLibrary::load_dir(dir);
  ASSERT_FALSE(library.has_value());
  EXPECT_EQ(library.error().code, cf::orch::error_codes::kPromptInvalid);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(PromptLibraryTest, UnterminatedPlaceholderIsRejected) {
  const auto dir = make_temp_dir();
  write_file(dir / "coder.md", "Prompt with {{brief.\n");
  auto library = cf::orch::PromptLibrary::load_dir(dir);
  ASSERT_FALSE(library.has_value());
  EXPECT_EQ(library.error().code, cf::orch::error_codes::kPromptInvalid);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(PromptLibraryTest, MissingDirectoryFails) {
  auto library = cf::orch::PromptLibrary::load_dir("/nonexistent/prompts");
  ASSERT_FALSE(library.has_value());
  EXPECT_EQ(library.error().code, cf::orch::error_codes::kPromptNotFound);
}

TEST(PromptLibraryTest, UnknownRoleFailsToRender) {
  const cf::orch::PromptLibrary library = cf::orch::PromptLibrary::defaults();
  auto rendered = library.render("architect", {});
  ASSERT_FALSE(rendered.has_value());
  EXPECT_EQ(rendered.error().code, cf::orch::error_codes::kPromptNotFound);
}

TEST(PromptLibraryTest, RepoPromptsDirectoryLoads) {
  // The checked-in prompts/ directory must stay valid.
  auto library = cf::orch::PromptLibrary::load_dir(
      std::filesystem::path(CF_SOURCE_DIR) / "prompts");
  ASSERT_TRUE(library.has_value()) << library.error().message;
  EXPECT_TRUE(library->has("coder"));
  EXPECT_TRUE(library->has("reviewer"));
  EXPECT_TRUE(library->has("qa"));
}

}  // namespace
