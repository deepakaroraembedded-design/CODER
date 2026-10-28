// Unit tests for reference-doc version matching (design section 6.3.1).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "cf/refmirror/versions.hpp"

namespace {

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_versions_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

cf::refmirror::ReferenceSet set(std::string id, std::string version) {
  cf::refmirror::ReferenceSet entry;
  entry.id = std::move(id);
  entry.version = std::move(version);
  entry.source = "doc.md";
  entry.sha256 = "abc";
  return entry;
}

TEST(VersionsTest, ReadsPinnedDependencyVersions) {
  const auto dir = make_temp_dir();
  write_file(dir / "vcpkg.json",
             R"({"dependencies": ["fmt", {"name": "nlohmann-json", "version>=": "3.12.0"}]})");
  write_file(dir / "requirements.txt", "requests==2.32.3\nnumpy>=2 ; python_version>='3'\n");
  write_file(dir / "package.json",
             R"({"dependencies": {"react": "^18.3.1"}, "devDependencies": {"vitest": "~2.0.5"}})");

  auto versions = cf::refmirror::dependency_versions(dir);
  ASSERT_TRUE(versions.has_value()) << versions.error().message;
  EXPECT_EQ(versions->at("nlohmann-json"), "3.12.0");
  EXPECT_EQ(versions->at("requests"), "2.32.3");
  EXPECT_EQ(versions->at("react"), "18.3.1");
  EXPECT_EQ(versions->at("vitest"), "2.0.5");
  EXPECT_EQ(versions->count("numpy"), 0u);  // a range is not a pin
  EXPECT_EQ(versions->count("fmt"), 0u);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(VersionsTest, SkipsSetsWhoseVersionDoesNotMatchTheProject) {
  cf::refmirror::Manifest manifest;
  manifest.sets = {set("nlohmann-json", "3.12.0"), set("boost", "1.89.0"),
                   set("cppreference", "2026-09")};
  std::map<std::string, std::string, std::less<>> deps;
  deps["nlohmann-json"] = "3.12.0";
  deps["boost"] = "1.84.0";

  cf::refmirror::VersionReport report;
  const auto matched = cf::refmirror::match_versions(manifest, deps, report);
  ASSERT_EQ(matched.sets.size(), 2u);
  EXPECT_EQ(matched.sets[0].id, "nlohmann-json");
  EXPECT_EQ(matched.sets[1].id, "cppreference");
  ASSERT_EQ(report.skipped.size(), 1u);
  EXPECT_EQ(report.skipped[0].id, "boost");
  EXPECT_EQ(report.skipped[0].pinned_version, "1.84.0");
  EXPECT_EQ(report.pinned.at("nlohmann-json"), "3.12.0");
  EXPECT_EQ(report.pinned.at("cppreference"), "2026-09");
}

}  // namespace
