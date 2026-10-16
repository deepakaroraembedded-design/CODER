// Unit tests for the review-validity rules (design section 7.4).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cf/orch/review_policy.hpp"

namespace {

using cf::orch::decide_review;
using cf::orch::FileChange;
using cf::orch::ReviewContext;
using cf::orch::ReviewKind;

FileChange code_change(uint32_t added = 10, uint32_t removed = 2) {
  FileChange change;
  change.path = "src/registry/registry.cpp";
  change.added_lines = added;
  change.removed_lines = removed;
  return change;
}

FileChange test_change() {
  FileChange change;
  change.path = "tests/registry_test.cpp";
  change.added_lines = 40;
  change.qa_owned = true;
  return change;
}

TEST(ReviewPolicyTest, NoChangesNeedsNoReview) {
  EXPECT_EQ(decide_review({}, ReviewContext{}), ReviewKind::None);
}

TEST(ReviewPolicyTest, FirstGateRunNeedsFullReview) {
  ReviewContext context;
  context.first_gate_run = true;
  EXPECT_EQ(decide_review({code_change()}, context), ReviewKind::Full);
}

TEST(ReviewPolicyTest, QaOwnedTestsNeedNoReview) {
  EXPECT_EQ(decide_review({test_change()}, ReviewContext{}), ReviewKind::None);
}

TEST(ReviewPolicyTest, FormattingOnlyNeedsNoReview) {
  FileChange change = code_change(50, 50);
  change.formatting_only = true;
  EXPECT_EQ(decide_review({change}, ReviewContext{}), ReviewKind::None);
}

TEST(ReviewPolicyTest, MixedTestAndFormattingChangesNeedNoReview) {
  FileChange formatting = code_change(5, 5);
  formatting.formatting_only = true;
  EXPECT_EQ(decide_review({test_change(), formatting}, ReviewContext{}),
            ReviewKind::None);
}

TEST(ReviewPolicyTest, PublicInterfaceChangeNeedsFullReview) {
  FileChange change = code_change(1, 1);
  change.public_interface = true;
  EXPECT_EQ(decide_review({change}, ReviewContext{}), ReviewKind::Full);
}

TEST(ReviewPolicyTest, CrossComponentChangeNeedsFullReview) {
  FileChange change = code_change(1, 1);
  change.cross_component = true;
  EXPECT_EQ(decide_review({change}, ReviewContext{}), ReviewKind::Full);
}

TEST(ReviewPolicyTest, AdrListedFileNeedsFullReview) {
  FileChange change = code_change(1, 1);
  change.adr_listed = true;
  EXPECT_EQ(decide_review({change}, ReviewContext{}), ReviewKind::Full);
}

TEST(ReviewPolicyTest, CumulativeChangeOverThresholdNeedsFullReview) {
  ReviewContext context;
  context.lines_since_last_full_review = 350;
  EXPECT_EQ(decide_review({code_change(60, 0)}, context), ReviewKind::Full);
}

TEST(ReviewPolicyTest, ExactlyAtThresholdStaysDelta) {
  ReviewContext context;
  context.lines_since_last_full_review = 300;
  EXPECT_EQ(decide_review({code_change(100, 0)}, context), ReviewKind::Delta);
}

TEST(ReviewPolicyTest, DependencyInterfaceChangeNeedsFullReview) {
  ReviewContext context;
  context.dependency_interface_changed = true;
  EXPECT_EQ(decide_review({code_change()}, context), ReviewKind::Full);
}

TEST(ReviewPolicyTest, DefectFixOnApprovedFileIsDeltaReview) {
  EXPECT_EQ(decide_review({code_change(15, 3)}, ReviewContext{}), ReviewKind::Delta);
}

TEST(ReviewPolicyTest, TestFilePlusSmallCodeChangeIsDeltaReview) {
  EXPECT_EQ(decide_review({test_change(), code_change(5, 1)}, ReviewContext{}),
            ReviewKind::Delta);
}

TEST(ReviewPolicyTest, CustomThresholdIsHonoured) {
  ReviewContext context;
  context.full_review_threshold = 100;
  context.lines_since_last_full_review = 90;
  EXPECT_EQ(decide_review({code_change(20, 0)}, context), ReviewKind::Full);
}

}  // namespace
