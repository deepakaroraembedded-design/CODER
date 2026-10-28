// cf-git: the repository layer (design sections 5.3, 6.8.3, 10.4.5, 6.12.4).
//
// Worktrees, snapshot commits with the required CF trailers, diffs for the
// review-validity rules, file hashes for review binding, integration merges
// and the trailer lookup used by crash recovery. Every git command runs as an
// argument list through the shared process executor (no shell).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/process.hpp"
#include "cf/common/result.hpp"

namespace cf::git {

namespace error_codes {
inline constexpr const char* kNotARepository = "GIT_NOT_A_REPOSITORY";
inline constexpr const char* kCommandFailed = "GIT_COMMAND_FAILED";
inline constexpr const char* kInvalidPath = "GIT_INVALID_PATH";
inline constexpr const char* kNoChanges = "GIT_NO_CHANGES";
inline constexpr const char* kNotFound = "GIT_NOT_FOUND";
inline constexpr const char* kMergeConflict = "GIT_MERGE_CONFLICT";
inline constexpr const char* kSigningKey = "GIT_SIGNING_KEY";
}  // namespace error_codes

// Required commit trailers (design section 10.4.5).
struct CommitTrailers {
  std::string run_id;                      // CF-Run
  std::string milestone_id;                // CF-Milestone
  std::vector<std::string> requirements;   // CF-Requirements
  std::string agents;                      // CF-Agents: coder=..; reviewer=..
  std::string gates;                       // CF-Gates
  std::string phase;                       // CF-Phase (snapshot recovery)
  std::string waivers = "none";            // CF-Waivers
  std::string op_id;                       // CF-Op: the creating operation
  std::string squashed_from;               // CF-Squashed-From (integration only)
};

struct FileChange {
  std::string path;
  char status = 'M';  // A added, M modified, D deleted, R renamed
  // For renames (status R): the path before the rename. Empty otherwise.
  std::string old_path;
  uint32_t added_lines = 0;
  uint32_t removed_lines = 0;
  bool binary = false;
};

class Repository {
 public:
  // Creates a new repository (git init -b main) with a local bot identity.
  static Result<Repository> create(const std::filesystem::path& path,
                                   process::ProcessExecutor& executor);

  static Result<Repository> open(const std::filesystem::path& path,
                                 process::ProcessExecutor& executor);

  const std::filesystem::path& path() const { return path_; }

  // Signs every commit created afterwards with the SSH private key at
  // `key_path` (design section 10.4.2: all CF commits are signed). The key
  // is read by git at commit time; a missing or unreadable file is rejected
  // here. An empty path disables signing (local runs without a key).
  Status set_signing_key(const std::filesystem::path& key_path);

  // --- worktrees (design sections 5.3, 6.13.1) -----------------------------
  // Adds a worktree at worktree_path on branch. When the branch already
  // exists it is checked out; otherwise it is created from start_point.
  Status create_worktree(const std::filesystem::path& worktree_path,
                         const std::string& branch,
                         const std::string& start_point = "HEAD");

  Status remove_worktree(const std::filesystem::path& worktree_path);

  Result<bool> branch_exists(const std::string& branch) const;

  // True when `ref` resolves (a branch, tag, remote-tracking ref or commit).
  Result<bool> ref_exists(const std::string& ref) const;

  // --- commits (design sections 6.8.3, 10.4.5) ------------------------------
  Result<std::string> head_sha(const std::filesystem::path& worktree,
                               const std::string& ref = "HEAD") const;

  // Stages exactly `paths` (relative and inside the worktree) and creates a
  // commit carrying the CF trailers. GIT_NO_CHANGES when nothing is staged.
  Result<std::string> commit_paths(const std::filesystem::path& worktree,
                                   const std::vector<std::string>& paths,
                                   const std::string& message,
                                   const CommitTrailers& trailers) const;

  // --- diffs (design sections 7.4, 10.4.6) ---------------------------------
  Result<std::vector<FileChange>> diff(const std::string& from_sha,
                                       const std::string& to_sha) const;

  // The blob hash of a file at a commit; a review verdict is bound to it.
  Result<std::string> file_hash(const std::string& commit_sha,
                                const std::string& path) const;

  // The blob size in bytes (STORE_NOT_FOUND when the path is absent).
  Result<uint64_t> file_size(const std::string& commit_sha,
                             const std::string& path) const;

  // The full commit message (subject, body and trailers).
  Result<std::string> commit_message(const std::string& commit_sha) const;

  // The raw content of a file at a commit (binary content is returned as-is).
  Result<std::string> file_content(const std::string& commit_sha,
                                   const std::string& path) const;

  // --- remotes (design sections 6.12.1, 10.4) ------------------------------
  // Runs a raw git command (argument list, no shell) with the repository as
  // the working directory; the escape hatch for push/ls-remote operations.
  Result<std::string> command(const std::vector<std::string>& args) const;

  // The sha of a remote branch, or an empty string when it does not exist.
  Result<std::string> remote_branch_sha(const std::string& remote,
                                        const std::string& branch) const;

  // Fetches one branch and updates its remote-tracking ref.
  Status fetch(const std::string& remote, const std::string& branch) const;

  // True when `ancestor` is an ancestor of `descendant` (git merge-base
  // --is-ancestor; a missing object is an error).
  Result<bool> is_ancestor(const std::string& ancestor,
                           const std::string& descendant) const;

  // --- recovery (design sections 6.12.4, 10.4.5) ---------------------------
  // Walks back from `ref` (at most max_walk commits) and returns the commit
  // whose <key> trailer equals value, e.g. ("CF-Op", "op_7f3a").
  Result<std::string> find_commit_by_trailer(const std::string& ref,
                                             const std::string& key,
                                             const std::string& value,
                                             std::size_t max_walk = 1000) const;

  // Creates or moves a ref (used to park recovered work under
  // refs/cf/recovered/... before a reset).
  Status update_ref(const std::string& ref, const std::string& sha) const;

  // Resets a worktree's checked-out branch to sha, discarding local changes.
  Status reset_hard(const std::filesystem::path& worktree,
                    const std::string& sha) const;

  // True when the worktree has staged or unstaged changes.
  Result<bool> is_dirty(const std::filesystem::path& worktree) const;

  // Paths with uncommitted changes in the worktree (added, modified,
  // deleted, untracked; a rename contributes both paths). Used by the
  // write-set enforcement (design section 6.13.2).
  Result<std::vector<std::string>> worktree_changes(
      const std::filesystem::path& worktree) const;

  // --- integration (design sections 6.13.3, 10.4.5) ------------------------
  // Squash-merges branch into the worktree's checked-out branch and commits
  // with the trailers. On conflict returns GIT_MERGE_CONFLICT and leaves the
  // merge state for the coder to resolve (abort_merge resets it).
  Result<std::string> merge_squash(const std::filesystem::path& worktree,
                                   const std::string& branch,
                                   const std::string& message,
                                   const CommitTrailers& trailers) const;

  Status abort_merge(const std::filesystem::path& worktree) const;

  // Rebases the worktree's checked-out branch onto `upstream`; on conflict
  // the caller should abort_rebase. Integration only (design section
  // 10.4.9).
  Status rebase(const std::filesystem::path& worktree,
                const std::string& upstream) const;
  Status abort_rebase(const std::filesystem::path& worktree) const;

  // The unmerged paths after a conflicted rebase/merge.
  Result<std::vector<std::string>> conflicted_files(
      const std::filesystem::path& worktree) const;
  // Stages every change in the worktree, including resolved conflicts.
  Status stage_all(const std::filesystem::path& worktree) const;
  // Continues an in-progress rebase without opening an editor.
  Status continue_rebase(const std::filesystem::path& worktree) const;

 private:
  Repository(std::filesystem::path path, process::ProcessExecutor& executor);

  process::ProcessResult run_raw(const std::vector<std::string>& args,
                                 const std::filesystem::path& cwd) const;

  // Runs a git command, requiring success; returns trimmed stdout.
  Result<std::string> run(const std::vector<std::string>& args,
                          const std::filesystem::path& cwd) const;

  Result<std::string> commit_staged(const std::filesystem::path& worktree,
                                    const std::string& message,
                                    const CommitTrailers& trailers) const;

  std::filesystem::path path_;
  process::ProcessExecutor& executor_;
  std::filesystem::path signing_key_;  // empty: unsigned commits
};

// Subject + blank line + the non-empty trailers, in the design's order.
std::string format_commit_message(const std::string& subject,
                                  const CommitTrailers& trailers);

}  // namespace cf::git
