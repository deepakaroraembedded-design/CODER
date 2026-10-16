#include "cf/git/repository.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace cf::git {
namespace {

std::string trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  return text.substr(begin);
}

bool is_safe_relative_path(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  const std::filesystem::path parsed(path);
  if (parsed.is_absolute() || parsed.has_root_name() || parsed.has_root_directory()) {
    return false;
  }
  for (const auto& component : parsed) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (true) {
    const std::size_t tab = line.find('\t', begin);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(begin));
      break;
    }
    fields.push_back(line.substr(begin, tab - begin));
    begin = tab + 1;
  }
  return fields;
}

constexpr const char* kUserName = "SwifTrade CF Bot";
constexpr const char* kUserEmail = "cf-bot@swiftradellc.com";

}  // namespace

std::string format_commit_message(const std::string& subject,
                                  const CommitTrailers& trailers) {
  std::string message = subject + "\n";
  std::string trailer_block;
  const auto add = [&trailer_block](const std::string& key, const std::string& value) {
    if (value.empty()) {
      return;
    }
    trailer_block += key + ": " + value + "\n";
  };
  add("CF-Run", trailers.run_id);
  add("CF-Milestone", trailers.milestone_id);
  if (!trailers.requirements.empty()) {
    std::string joined;
    for (std::size_t i = 0; i < trailers.requirements.size(); ++i) {
      if (i > 0) {
        joined += ", ";
      }
      joined += trailers.requirements[i];
    }
    add("CF-Requirements", joined);
  }
  add("CF-Agents", trailers.agents);
  add("CF-Gates", trailers.gates);
  add("CF-Waivers", trailers.waivers);
  add("CF-Op", trailers.op_id);
  add("CF-Squashed-From", trailers.squashed_from);
  if (!trailer_block.empty()) {
    message += "\n" + trailer_block;
  }
  return message;
}

Repository::Repository(std::filesystem::path path, process::ProcessExecutor& executor)
    : path_(std::move(path)), executor_(executor) {}

process::ProcessResult Repository::run_raw(const std::vector<std::string>& args,
                                           const std::filesystem::path& cwd) const {
  // `-C` is resolved by git relative to the process working directory, which
  // is also `cwd`; a relative `cwd` would therefore resolve to `cwd/cwd`.
  // Absolutize once so relative callers behave like absolute ones.
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(cwd, ec);
  const std::string directory = ec ? cwd.string() : absolute.string();
  process::ProcessRequest request;
  request.argv.push_back("git");
  request.argv.push_back("-C");
  request.argv.push_back(directory);
  request.argv.insert(request.argv.end(), args.begin(), args.end());
  request.working_directory = directory;
  request.timeout = std::chrono::milliseconds(60000);
  return executor_.run(request);
}

Result<std::string> Repository::run(const std::vector<std::string>& args,
                                    const std::filesystem::path& cwd) const {
  const process::ProcessResult result = run_raw(args, cwd);
  if (!result.launched) {
    return fail(error_codes::kCommandFailed,
                "cannot run git: " + result.error);
  }
  if (result.exit_code != 0) {
    std::string detail = trim(result.stderr_text);
    if (detail.empty()) {
      detail = trim(result.stdout_text);
    }
    return fail(error_codes::kCommandFailed,
                "git " + (args.empty() ? std::string() : args.front()) +
                    " failed (exit " + std::to_string(result.exit_code) + "): " + detail);
  }
  return trim(result.stdout_text);
}

Result<Repository> Repository::create(const std::filesystem::path& path,
                                      process::ProcessExecutor& executor) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return fail(error_codes::kCommandFailed,
                "cannot create repository directory " + path.string() + ": " +
                    ec.message());
  }
  Repository repository(path, executor);
  auto initialized = repository.run({"init", "-b", "main"}, path);
  if (!initialized) {
    return tl::make_unexpected(initialized.error());
  }
  auto name = repository.run({"config", "user.name", kUserName}, path);
  if (!name) {
    return tl::make_unexpected(name.error());
  }
  auto email = repository.run({"config", "user.email", kUserEmail}, path);
  if (!email) {
    return tl::make_unexpected(email.error());
  }
  return repository;
}

Result<Repository> Repository::open(const std::filesystem::path& path,
                                    process::ProcessExecutor& executor) {
  Repository repository(path, executor);
  const process::ProcessResult result =
      repository.run_raw({"rev-parse", "--git-dir"}, path);
  if (!result.launched || result.exit_code != 0) {
    return fail(error_codes::kNotARepository,
                "not a git repository: " + path.string());
  }
  return repository;
}

Result<bool> Repository::branch_exists(const std::string& branch) const {
  const process::ProcessResult result =
      run_raw({"rev-parse", "--verify", "--quiet", "refs/heads/" + branch}, path_);
  if (!result.launched) {
    return fail(error_codes::kCommandFailed, "cannot run git: " + result.error);
  }
  return result.exit_code == 0;
}

Status Repository::create_worktree(const std::filesystem::path& worktree_path,
                                   const std::string& branch,
                                   const std::string& start_point) {
  std::error_code ec;
  std::filesystem::create_directories(worktree_path.parent_path(), ec);
  if (ec) {
    return fail(error_codes::kCommandFailed,
                "cannot create worktree parent directory: " + ec.message());
  }
  auto exists = branch_exists(branch);
  if (!exists) {
    return tl::make_unexpected(exists.error());
  }
  if (*exists) {
    auto added = run({"worktree", "add", worktree_path.string(), branch}, path_);
    if (!added) {
      return tl::make_unexpected(added.error());
    }
    return {};
  }
  auto added = run({"worktree", "add", "-b", branch, worktree_path.string(), start_point},
                   path_);
  if (!added) {
    return tl::make_unexpected(added.error());
  }
  return {};
}

Status Repository::remove_worktree(const std::filesystem::path& worktree_path) {
  auto removed = run({"worktree", "remove", "--force", worktree_path.string()}, path_);
  if (!removed) {
    return tl::make_unexpected(removed.error());
  }
  return {};
}

Result<std::string> Repository::head_sha(const std::filesystem::path& worktree,
                                         const std::string& ref) const {
  return run({"rev-parse", ref}, worktree);
}

Result<std::string> Repository::commit_paths(
    const std::filesystem::path& worktree, const std::vector<std::string>& paths,
    const std::string& message, const CommitTrailers& trailers) const {
  if (paths.empty()) {
    return fail(error_codes::kInvalidPath, "no paths to commit");
  }
  for (const auto& path : paths) {
    if (!is_safe_relative_path(path)) {
      return fail(error_codes::kInvalidPath,
                  "path must be relative and inside the worktree: '" + path + "'");
    }
  }
  std::vector<std::string> add_args = {"add", "--"};
  add_args.insert(add_args.end(), paths.begin(), paths.end());
  auto added = run(add_args, worktree);
  if (!added) {
    return tl::make_unexpected(added.error());
  }

  const process::ProcessResult staged = run_raw({"diff", "--cached", "--quiet"}, worktree);
  if (!staged.launched) {
    return fail(error_codes::kCommandFailed, "cannot run git: " + staged.error);
  }
  if (staged.exit_code == 0) {
    return fail(error_codes::kNoChanges, "nothing staged to commit in " +
                                             worktree.string());
  }
  return commit_staged(worktree, message, trailers);
}

Result<std::string> Repository::commit_staged(
    const std::filesystem::path& worktree, const std::string& message,
    const CommitTrailers& trailers) const {
  const std::string full_message = format_commit_message(message, trailers);
  auto committed = run({"-c", std::string("user.name=") + kUserName, "-c",
                        std::string("user.email=") + kUserEmail, "commit", "-m",
                        full_message},
                       worktree);
  if (!committed) {
    return tl::make_unexpected(committed.error());
  }
  return head_sha(worktree);
}

Result<std::vector<FileChange>> Repository::diff(const std::string& from_sha,
                                                 const std::string& to_sha) const {
  auto numstat = run({"diff", "--numstat", from_sha, to_sha}, path_);
  if (!numstat) {
    return tl::make_unexpected(numstat.error());
  }
  auto name_status = run({"diff", "--name-status", from_sha, to_sha}, path_);
  if (!name_status) {
    return tl::make_unexpected(name_status.error());
  }

  std::vector<FileChange> changes;
  for (const auto& line : split_lines(*numstat)) {
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() < 3) {
      continue;
    }
    FileChange change;
    change.path = fields[2];
    if (fields[0] == "-" || fields[1] == "-") {
      change.binary = true;
    } else {
      change.added_lines = static_cast<uint32_t>(std::stoul(fields[0]));
      change.removed_lines = static_cast<uint32_t>(std::stoul(fields[1]));
    }
    changes.push_back(std::move(change));
  }

  // Status letters; renames report the new path.
  std::vector<std::pair<std::string, char>> statuses;
  for (const auto& line : split_lines(*name_status)) {
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() < 2) {
      continue;
    }
    const char status = fields[0].empty() ? 'M' : fields[0][0];
    const std::string path = fields.size() >= 3 ? fields[2] : fields[1];
    statuses.emplace_back(path, status);
  }
  for (auto& change : changes) {
    for (const auto& [path, status] : statuses) {
      if (path == change.path) {
        change.status = status;
        break;
      }
    }
  }
  std::sort(changes.begin(), changes.end(),
            [](const FileChange& a, const FileChange& b) { return a.path < b.path; });
  return changes;
}

Result<std::string> Repository::file_hash(const std::string& commit_sha,
                                          const std::string& path) const {
  if (!is_safe_relative_path(path)) {
    return fail(error_codes::kInvalidPath,
                "path must be relative and inside the repository: '" + path + "'");
  }
  auto blob = run({"rev-parse", commit_sha + ":" + path}, path_);
  if (!blob) {
    return fail(error_codes::kNotFound,
                "no file '" + path + "' at commit " + commit_sha);
  }
  return blob;
}

Result<uint64_t> Repository::file_size(const std::string& commit_sha,
                                       const std::string& path) const {
  if (!is_safe_relative_path(path)) {
    return fail(error_codes::kInvalidPath,
                "path must be relative and inside the repository: '" + path + "'");
  }
  auto size = run({"cat-file", "-s", commit_sha + ":" + path}, path_);
  if (!size) {
    return fail(error_codes::kNotFound,
                "no file '" + path + "' at commit " + commit_sha);
  }
  try {
    return static_cast<uint64_t>(std::stoull(*size));
  } catch (const std::exception&) {
    return fail(error_codes::kCommandFailed, "unexpected git cat-file output: " + *size);
  }
}

Result<std::string> Repository::commit_message(const std::string& commit_sha) const {
  return run({"log", "-1", "--format=%B", commit_sha}, path_);
}

Result<std::string> Repository::file_content(const std::string& commit_sha,
                                             const std::string& path) const {
  if (!is_safe_relative_path(path)) {
    return fail(error_codes::kInvalidPath,
                "path must be relative and inside the repository: '" + path + "'");
  }
  // Raw output: trimming would corrupt the content.
  const process::ProcessResult result =
      run_raw({"show", commit_sha + ":" + path}, path_);
  if (!result.launched) {
    return fail(error_codes::kCommandFailed, "cannot run git: " + result.error);
  }
  if (result.exit_code != 0) {
    return fail(error_codes::kNotFound,
                "no file '" + path + "' at commit " + commit_sha);
  }
  return result.stdout_text;
}

Result<std::string> Repository::command(const std::vector<std::string>& args) const {
  return run(args, path_);
}

Result<std::string> Repository::remote_branch_sha(const std::string& remote,
                                                  const std::string& branch) const {
  auto output = run({"ls-remote", remote, "refs/heads/" + branch}, path_);
  if (!output) {
    return tl::make_unexpected(output.error());
  }
  if (output->empty()) {
    return std::string();  // branch does not exist remotely
  }
  const std::size_t tab = output->find_first_of(" \t");
  return tab == std::string::npos ? *output : output->substr(0, tab);
}

Result<std::string> Repository::find_commit_by_trailer(const std::string& ref,
                                                       const std::string& key,
                                                       const std::string& value,
                                                       std::size_t max_walk) const {
  auto log = run({"log", "-n", std::to_string(max_walk),
                  "--format=%H%x09%(trailers:key=" + key + ",valueonly)", ref},
                 path_);
  if (!log) {
    return tl::make_unexpected(log.error());
  }
  for (const auto& line : split_lines(*log)) {
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() < 2) {
      continue;
    }
    if (trim(fields[1]) == value) {
      return fields[0];
    }
  }
  return fail(error_codes::kNotFound,
              "no commit with trailer " + key + "=" + value + " within " +
                  std::to_string(max_walk) + " commits of " + ref);
}

Result<std::string> Repository::merge_squash(const std::filesystem::path& worktree,
                                             const std::string& branch,
                                             const std::string& message,
                                             const CommitTrailers& trailers) const {
  const process::ProcessResult merged = run_raw({"merge", "--squash", branch}, worktree);
  if (!merged.launched) {
    return fail(error_codes::kCommandFailed, "cannot run git: " + merged.error);
  }
  if (merged.exit_code != 0) {
    return fail(error_codes::kMergeConflict,
                "squash merge of " + branch + " conflicted: " +
                    trim(merged.stderr_text));
  }
  const process::ProcessResult staged = run_raw({"diff", "--cached", "--quiet"}, worktree);
  if (!staged.launched) {
    return fail(error_codes::kCommandFailed, "cannot run git: " + staged.error);
  }
  if (staged.exit_code == 0) {
    return fail(error_codes::kNoChanges,
                "branch " + branch + " is already merged into " + worktree.string());
  }
  return commit_staged(worktree, message, trailers);
}

Status Repository::update_ref(const std::string& ref, const std::string& sha) const {
  auto updated = run({"update-ref", ref, sha}, path_);
  if (!updated) {
    return tl::make_unexpected(updated.error());
  }
  return {};
}

Status Repository::reset_hard(const std::filesystem::path& worktree,
                              const std::string& sha) const {
  auto reset = run({"reset", "--hard", sha}, worktree);
  if (!reset) {
    return tl::make_unexpected(reset.error());
  }
  return {};
}

Result<bool> Repository::is_dirty(const std::filesystem::path& worktree) const {
  const process::ProcessResult status = run_raw({"status", "--porcelain"}, worktree);
  if (!status.launched) {
    return fail(error_codes::kCommandFailed, "cannot run git: " + status.error);
  }
  if (status.exit_code != 0) {
    return fail(error_codes::kCommandFailed, trim(status.stderr_text));
  }
  return !trim(status.stdout_text).empty();
}

Status Repository::abort_merge(const std::filesystem::path& worktree) const {
  // A squash merge leaves no MERGE_HEAD, so `merge --abort` refuses; fall
  // back to a hard reset, which restores the worktree either way.
  const process::ProcessResult aborted = run_raw({"merge", "--abort"}, worktree);
  if (aborted.launched && aborted.exit_code == 0) {
    return {};
  }
  (void)run_raw({"reset", "--hard", "HEAD"}, worktree);
  return {};
}

}  // namespace cf::git
