#include "cf/orch/review_policy.hpp"

#include <algorithm>

namespace cf::orch {

ReviewKind decide_review(const std::vector<FileChange>& changes,
                         const ReviewContext& context) {
  if (changes.empty()) {
    return ReviewKind::None;
  }

  // Only QA-owned test files and formatting-only edits: QA owns tests and a
  // formatting change does not alter the syntax tree, so the review stands.
  const bool qa_or_formatting_only =
      std::all_of(changes.begin(), changes.end(),
                  [](const FileChange& change) {
                    return change.qa_owned || change.formatting_only;
                  });
  if (qa_or_formatting_only) {
    return ReviewKind::None;
  }

  if (context.first_gate_run) {
    return ReviewKind::Full;
  }

  const bool touches_boundary =
      std::any_of(changes.begin(), changes.end(), [](const FileChange& change) {
        return change.public_interface || change.cross_component || change.adr_listed;
      });
  if (touches_boundary) {
    return ReviewKind::Full;
  }

  uint32_t changed_lines = 0;
  for (const auto& change : changes) {
    changed_lines += change.added_lines + change.removed_lines;
  }
  if (context.lines_since_last_full_review + changed_lines >
      context.full_review_threshold) {
    return ReviewKind::Full;
  }

  if (context.dependency_interface_changed) {
    return ReviewKind::Full;
  }

  // A fix to already-approved files, or resolved rebase conflicts: only the
  // changes since the approved commit are reviewed.
  return ReviewKind::Delta;
}

}  // namespace cf::orch
