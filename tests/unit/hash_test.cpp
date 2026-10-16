#include <gtest/gtest.h>

#include <string>

#include "cf/common/hash.hpp"

TEST(HashTest, Sha256KnownVectors) {
  EXPECT_EQ(cf::sha256_hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(cf::sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HashTest, Sha256HexLength) {
  EXPECT_EQ(cf::sha256_hex("coder-framework").size(), 64u);
}

TEST(HashTest, IdempotencyKeyIsDeterministic) {
  EXPECT_EQ(cf::idempotency_key("R12", "M3", "push", "digest"),
            cf::idempotency_key("R12", "M3", "push", "digest"));
}

TEST(HashTest, IdempotencyKeyDiscriminatesInputs) {
  const std::string base = cf::idempotency_key("R12", "M3", "push", "digest");
  EXPECT_NE(base, cf::idempotency_key("R12", "M3", "push", "other"));
  EXPECT_NE(base, cf::idempotency_key("R12", "M4", "push", "digest"));
  EXPECT_NE(base, cf::idempotency_key("R13", "M3", "push", "digest"));
  EXPECT_NE(base, cf::idempotency_key("R12", "M3", "pr_create", "digest"));
  EXPECT_EQ(base.size(), 64u);
}
