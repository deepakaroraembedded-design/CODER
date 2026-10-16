// cf-orch: review-validity rules (design section 7.4).
//
// A review verdict is bound to the exact commit it reviewed and to the
// content hash of each file it covered. After any later change, the
// orchestrator compares the new commit with the last approved one and decides
// whether a full review, a delta review, or no review is needed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cf::orch {

// One changed file since the last approved commit.
struct FileChange {
  std::string path;
  uint32_t added_lines = 0;
  uint32_t removed_lines = 0;
  bool qa_owned = false;         // tests/ or qa/ (QA owns test files)
  bool formatting_only = false;  // the syntax tree is unchanged
  bool public_interface = false; // a public interface header
  bool cross_component = false;  // crosses a component boundary
  bool adr_listed = false;       // listed in a design decision record
};

enum class ReviewKind {
  None,   // the review stays valid
  Delta,  // review only the changes since the approved commit
  Full,   // review the milestone's whole diff
};

struct ReviewContext {
  bool first_gate_run = false;  // first passing gate run for the milestone
  bool dependency_interface_changed = false;  // an interface it depends on changed (6.13.3)
  // Cumulative changed lines since the last full review, not counting the
  // current change set.
  uint32_t lines_since_last_full_review = 0;
  // review.diff_threshold_lines (design section 15).
  uint32_t full_review_threshold = 400;
};

// Applies the table in design section 7.4. A clean rebase that leaves the
// milestone's content unchanged produces no FileChange entries and therefore
// no review; a rebase that resolved conflicts or picked up upstream changes
// to covered files appears as changes and is reviewed accordingly.
ReviewKind decide_review(const std::vector<FileChange>& changes,
                         const ReviewContext& context);

}  // namespace cf::orch
