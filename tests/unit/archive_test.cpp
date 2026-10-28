// Unit tests for the encrypted archive format shared by cf-llm and cf-agents.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "cf/common/archive.hpp"

namespace {

namespace fs = std::filesystem;

fs::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = fs::temp_directory_path() /
                   ("cf_archive_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  fs::create_directories(dir);
  return dir;
}

constexpr const char* kKeyEnv = "CF_ARCHIVE_TEST_KEY";
constexpr const char* kKey =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class ArchiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    ::setenv(kKeyEnv, kKey, 1);
  }
  void TearDown() override {
    ::unsetenv(kKeyEnv);
    std::error_code error;
    fs::remove_all(dir_, error);
  }
  fs::path dir_;
};

TEST_F(ArchiveTest, RoundTripsAndRejectsTheWrongKey) {
  auto key = cf::common::archive::key_from_env(kKeyEnv);
  ASSERT_TRUE(key.has_value());
  const fs::path file = dir_ / "nested" / "call.cfar";
  const std::string plaintext = R"({"hello":"world","n":1})";
  ASSERT_TRUE(cf::common::archive::write_encrypted(file, *key, plaintext));
  auto read = cf::common::archive::read_encrypted(file, *key);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, plaintext);

  cf::common::archive::Key wrong(32, 0x42);
  EXPECT_FALSE(cf::common::archive::read_encrypted(file, wrong).has_value());
}

TEST_F(ArchiveTest, GeneratesPrefixedRandomIds) {
  const std::string first = cf::common::archive::new_id("call_");
  const std::string second = cf::common::archive::new_id("call_");
  EXPECT_EQ(first.rfind("call_", 0), 0u);
  EXPECT_EQ(first.size(), 5u + 16u);
  EXPECT_NE(first, second);
}

}  // namespace
