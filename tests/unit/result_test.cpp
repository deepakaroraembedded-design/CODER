// Unit tests for cf::Result and the error contract (design section 4.1).
#include <string>

#include <gtest/gtest.h>

#include "cf/common/result.hpp"

namespace {

cf::Result<int> parse_positive(int value) {
  if (value <= 0) {
    return cf::fail("TEST_NON_POSITIVE", "value must be positive");
  }
  return value;
}

cf::Status always_ok() { return {}; }

TEST(ResultTest, ValueRoundTrip) {
  auto result = parse_positive(41);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 41);
}

TEST(ResultTest, ErrorRoundTrip) {
  auto result = parse_positive(0);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, "TEST_NON_POSITIVE");
  EXPECT_FALSE(result.error().message.empty());
}

TEST(ResultTest, MapAndAndThen) {
  auto doubled = parse_positive(21).map([](int v) { return v * 2; });
  ASSERT_TRUE(doubled.has_value());
  EXPECT_EQ(*doubled, 42);

  auto chained = parse_positive(21).and_then(
      [](int v) -> cf::Result<std::string> { return std::to_string(v); });
  ASSERT_TRUE(chained.has_value());
  EXPECT_EQ(*chained, "21");

  auto failed_chain = parse_positive(-1).and_then(
      [](int v) -> cf::Result<std::string> { return std::to_string(v); });
  ASSERT_FALSE(failed_chain.has_value());
  EXPECT_EQ(failed_chain.error().code, "TEST_NON_POSITIVE");
}

TEST(ResultTest, VoidStatus) {
  EXPECT_TRUE(always_ok().has_value());
}

}  // namespace
