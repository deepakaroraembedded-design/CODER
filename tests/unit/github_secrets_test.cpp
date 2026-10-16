// Unit tests for the secret scanner (design section 10.4.6 check 4).
#include <gtest/gtest.h>

#include <string>

#include "cf/github/secrets.hpp"

namespace {

TEST(GithubSecretsTest, DetectsCommonSecretPatterns) {
  const std::string text =
      "aws_key = AKIAIOSFODNN7EXAMPLE\n"
      "github = ghp_abcdefghijklmnopqrstuvwxyz0123456789\n"
      "-----BEGIN RSA PRIVATE KEY-----\n"
      "slack = xoxb-1234567890-abcdefghij\n"
      "password: \"hunter2hunter2\"\n";
  const auto findings = cf::github::scan_for_secrets(text);
  ASSERT_EQ(findings.size(), 5u);
  EXPECT_EQ(findings[0].pattern, "aws-access-key");
  EXPECT_EQ(findings[0].line, 1);
  EXPECT_EQ(findings[1].pattern, "github-token");
  EXPECT_EQ(findings[1].line, 2);
  EXPECT_EQ(findings[2].pattern, "private-key");
  EXPECT_EQ(findings[2].line, 3);
  EXPECT_EQ(findings[3].pattern, "slack-token");
  EXPECT_EQ(findings[3].line, 4);
  EXPECT_EQ(findings[4].pattern, "generic-secret");
  EXPECT_EQ(findings[4].line, 5);
}

TEST(GithubSecretsTest, FindingsNeverCarryTheSecret) {
  const auto findings =
      cf::github::scan_for_secrets("key = AKIAIOSFODNN7EXAMPLE\n");
  ASSERT_FALSE(findings.empty());
  // The finding carries only a pattern name and a line number.
  EXPECT_EQ(findings[0].pattern, "aws-access-key");
  EXPECT_EQ(findings[0].line, 1);
}

TEST(GithubSecretsTest, CleanTextHasNoFindings) {
  const auto findings = cf::github::scan_for_secrets(
      "int main() { return 0; }\n// password handling lives in the vault\n");
  EXPECT_TRUE(findings.empty());
}

TEST(GithubSecretsTest, ShortGenericValuesAreNotFlagged) {
  const auto findings =
      cf::github::scan_for_secrets("token: \"short\"\napi_key = \"x\"\n");
  EXPECT_TRUE(findings.empty());
}

}  // namespace
