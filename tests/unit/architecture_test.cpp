// Unit tests for the config-driven architecture/layering gate (design
// sections 6.5, 6.7 stage 2).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "cf/gates/architecture.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_arch_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

TEST(ArchitectureGateTest, FlagsForbiddenImportsOnly) {
  const auto dir = make_temp_dir();
  write_file(dir / "src" / "ui" / "view.cpp",
             "#include \"src/storage/db.hpp\"\n"
             "#include <vector>\n"
             "int view() { return 0; }\n");
  write_file(dir / "src" / "ui" / "clean.cpp", "int clean() { return 0; }\n");
  write_file(dir / "src" / "storage" / "db.hpp", "#pragma once\n");
  write_file(dir / "src" / "ui" / "build" / "generated.cpp",
             "#include \"src/storage/db.hpp\"\n");
  const std::vector<cf::gates::ArchitectureRule> rules = {
      {"ARCH-1", "src/ui/", "src/storage/"}};
  auto findings = cf::gates::check_architecture_rules(dir, rules);
  ASSERT_TRUE(findings.has_value()) << findings.error().message;
  ASSERT_EQ(findings->size(), 1u);
  EXPECT_EQ(findings->front().gate, "architecture");
  EXPECT_NE(findings->front().message.find("ARCH-1"), std::string::npos);
  EXPECT_NE(findings->front().message.find("view.cpp"), std::string::npos);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(ArchitectureGateTest, NonImportMentionsAreNotViolations) {
  const auto dir = make_temp_dir();
  write_file(dir / "src" / "ui" / "comment.cpp",
             "// src/storage/db.hpp is mentioned but not imported\n"
             "int ok() { return 0; }\n");
  const std::vector<cf::gates::ArchitectureRule> rules = {
      {"ARCH-1", "src/ui/", "src/storage/"}};
  auto findings = cf::gates::check_architecture_rules(dir, rules);
  ASSERT_TRUE(findings.has_value());
  EXPECT_TRUE(findings->empty());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
