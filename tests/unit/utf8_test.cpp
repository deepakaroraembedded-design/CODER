#include <gtest/gtest.h>

#include <string>

#include "cf/common/utf8.hpp"

TEST(Utf8Test, PreservesValidAsciiAndUnicode) {
  const std::string valid = "hello\nh\xC3\xA9llo \xE2\x98\x83 \xF0\x9F\x98\x80";
  EXPECT_EQ(cf::sanitize_utf8(valid), valid);
}

TEST(Utf8Test, ReplacesInvalidAndTruncatedSequences) {
  EXPECT_EQ(cf::sanitize_utf8("\xFF"), "\xEF\xBF\xBD");
  EXPECT_EQ(cf::sanitize_utf8("a\xE2\x82"), "a\xEF\xBF\xBD");
  EXPECT_EQ(cf::sanitize_utf8("a\xC0\xAF" "b"),
            "a\xEF\xBF\xBD\xEF\xBF\xBD" "b");
}

TEST(Utf8Test, RejectsSurrogatesAndOutOfRangeValues) {
  EXPECT_EQ(cf::sanitize_utf8("\xED\xA0\x80"), "\xEF\xBF\xBD");
  EXPECT_EQ(cf::sanitize_utf8("\xF4\x90\x80\x80"), "\xEF\xBF\xBD");
}

TEST(Utf8Test, KeepsValidTextAroundInvalidBytes) {
  EXPECT_EQ(cf::sanitize_utf8("ok\xFFok"), "ok\xEF\xBF\xBDok");
}
