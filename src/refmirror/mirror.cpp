#include "cf/refmirror/mirror.hpp"

#include <algorithm>
#include <cctype>
#include <fnmatch.h>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>
#include <utility>

#include <nlohmann/json.hpp>

#include "cf/common/hash.hpp"
#include "cf/rag/html.hpp"
#include "cf/rag/injection_scan.hpp"

namespace cf::refmirror {
namespace {

using Json = nlohmann::json;

bool is_remote(std::string_view source) {
  return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  const std::string_view tail = text.substr(text.size() - suffix.size());
  return std::equal(tail.begin(), tail.end(), suffix.begin(), suffix.end(),
                    [](char left, char right) {
                      return std::tolower(static_cast<unsigned char>(left)) ==
                             std::tolower(static_cast<unsigned char>(right));
                    });
}

std::string archive_suffix(std::string_view source) {
  const std::size_t query = source.find_first_of("?#");
  if (query != std::string_view::npos) {
    source = source.substr(0, query);
  }
  for (const std::string_view suffix :
       {".tar.gz", ".tar.bz2", ".tar.xz", ".tgz", ".tbz2", ".txz", ".tar", ".zip"}) {
    if (ends_with_ci(source, suffix)) {
      return std::string(suffix);
    }
  }
  return {};
}

bool matches_extract_pattern(const std::filesystem::path& relative,
                             std::string_view pattern) {
  const std::string text = relative.generic_string();
  std::filesystem::path candidate(text);
  for (;;) {
    if (fnmatch(std::string(pattern).c_str(), candidate.generic_string().c_str(),
                0) == 0) {
      return true;
    }
    const std::filesystem::path parent = candidate.parent_path();
    if (parent.empty() || parent == candidate) {
      return false;
    }
    candidate = parent;
  }
}

std::vector<std::filesystem::path> extract_document_files(
    const std::filesystem::path& root, const std::vector<std::string>& patterns) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) {
      continue;
    }
    const std::string extension = iterator->path().extension().string();
    if (extension != ".html" && extension != ".htm" && extension != ".md" &&
        extension != ".markdown" && extension != ".txt") {
      continue;
    }
    std::error_code relative_error;
    const std::filesystem::path relative =
        std::filesystem::relative(iterator->path(), root, relative_error);
    if (relative_error) {
      continue;
    }
    for (const auto& pattern : patterns) {
      if (matches_extract_pattern(relative, pattern)) {
        files.push_back(iterator->path());
        break;
      }
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

struct TempDirectory {
  std::filesystem::path path;
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

bool run_command(process::ProcessExecutor& executor,
                 const std::vector<std::string>& argv,
                 const std::filesystem::path& working_directory,
                 std::chrono::milliseconds timeout) {
  process::ProcessRequest request;
  request.argv = argv;
  request.working_directory = working_directory.string();
  request.timeout = timeout;
  const process::ProcessResult result = executor.run(request);
  return result.launched && !result.timed_out && result.exit_code == 0;
}

bool run_extractor(process::ProcessExecutor& executor,
                   const std::vector<std::string>& argv,
                   const std::filesystem::path& working_directory,
                   std::chrono::milliseconds timeout) {
  return run_command(executor, argv, working_directory, timeout);
}

struct GeneratedDocument {
  std::string path;
  std::string text;
};

bool looks_like_html(std::string_view content);

Result<std::vector<GeneratedDocument>> generate_doxygen(
    const ReferenceSet& set, const MirrorOptions& options,
    process::ProcessExecutor& executor) {
  if (set.repo.empty()) {
    return fail("REFMIRROR_GENERATION_FAILED", "doxygen set has no repo");
  }
  TempDirectory temporary{options.out_dir / (".gen-" + set.id + "-" + set.version)};
  std::error_code error;
  const auto clone = temporary.path / "repo";
  std::filesystem::create_directories(temporary.path, error);
  if (error) {
    return fail("REFMIRROR_GENERATION_FAILED", error.message());
  }
  std::vector<std::string> clone_argv = {options.git_binary, "clone", "--depth", "1"};
  if (!set.ref.empty()) {
    clone_argv.push_back("--branch");
    clone_argv.push_back(set.ref);
  }
  clone_argv.push_back(set.repo);
  clone_argv.push_back(clone.string());
  if (!run_command(executor, clone_argv, temporary.path, options.timeout)) {
    return fail("REFMIRROR_GENERATION_FAILED", "cannot clone " + set.repo);
  }
  const auto html = temporary.path / "documentation";
  const auto doxyfile = temporary.path / "Doxyfile";
  {
    std::ofstream out(doxyfile);
    if (!out) {
      return fail("REFMIRROR_GENERATION_FAILED", "cannot write Doxyfile");
    }
    out << "PROJECT_NAME = " << set.id << "\n"
        << "OUTPUT_DIRECTORY = " << html.string() << "\n"
        << "INPUT = " << clone.string() << "\n"
        << "RECURSIVE = YES\n"
        << "GENERATE_HTML = YES\n"
        << "GENERATE_LATEX = NO\n"
        << "GENERATE_XML = NO\n"
        << "HTML_OUTPUT = html\n"
        << "QUIET = YES\n"
        << "WARN_IF_UNDOCUMENTED = NO\n";
  }
  if (!run_command(executor, {options.doxygen_binary, doxyfile.string()},
                   temporary.path, options.timeout)) {
    return fail("REFMIRROR_GENERATION_FAILED", "doxygen failed for " + set.id);
  }
  const auto html_root = html / "html";
  const auto files = extract_document_files(html_root, {"*"});
  if (files.empty()) {
    return fail("REFMIRROR_GENERATION_FAILED",
                "doxygen produced no HTML documents for " + set.id);
  }
  std::vector<GeneratedDocument> documents;
  for (const auto& file : files) {
    std::ifstream input(file, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
    const std::string cleaned =
        looks_like_html(raw) ? rag::clean_html_to_markdown(raw) : raw;
    std::error_code relative_error;
    const auto relative =
        std::filesystem::relative(file, html_root, relative_error);
    documents.push_back(GeneratedDocument{
        relative_error ? file.filename().string() : relative.generic_string(),
        cleaned});
  }
  return documents;
}

bool looks_like_html(std::string_view content) {
  const std::string lowered = [&content] {
    std::string out(content.substr(0, std::min<std::size_t>(content.size(), 512)));
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  }();
  return lowered.find("<!doctype html") != std::string::npos ||
         lowered.find("<html") != std::string::npos;
}

Result<std::string> fetch(const ReferenceSet& set, const MirrorOptions& options,
                          llm::HttpTransport& transport) {
  if (is_remote(set.source)) {
    llm::HttpRequest request;
    request.url = set.source;
    request.timeout = options.timeout;
    auto response = transport.get(request);
    if (!response) {
      return fail("REFMIRROR_FETCH_FAILED", response.error().message);
    }
    if (response->status < 200 || response->status >= 300) {
      return fail("REFMIRROR_FETCH_FAILED",
                  "HTTP " + std::to_string(response->status) + " for " + set.source);
    }
    return response->body;
  }
  std::ifstream in(set.source, std::ios::binary);
  if (!in) {
    return fail("REFMIRROR_FETCH_FAILED", "cannot read local source: " + set.source);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string front_matter(const ReferenceSet& set,
                         std::string_view archive_path = {}) {
  std::string out = "---\n";
  out += "doc_id: " + set.id + "\n";
  out += "version: \"" + set.version + "\"\n";
  out += "type: external_reference\n";
  out += "owner: reference-mirror\n";
  out += "classification: public\n";
  out += "scope: company\n";
  out += "status: active\n";
  if (!set.source.empty()) {
    out += "source: \"" + set.source + "\"\n";
  }
  if (!set.license.empty()) {
    out += "license: \"" + set.license + "\"\n";
  }
  if (!archive_path.empty()) {
    out += "archive_path: \"" + std::string(archive_path) + "\"\n";
  }
  out += "---\n\n";
  return out;
}

}  // namespace

bool MirrorReport::ok() const {
  return std::all_of(sets.begin(), sets.end(),
                     [](const SetReport& set) { return set.status == "ok"; });
}

std::string MirrorReport::to_json() const {
  Json json_sets = Json::array();
  for (const auto& set : sets) {
    json_sets.push_back(Json{{"id", set.id},
                             {"version", set.version},
                             {"status", set.status},
                             {"detail", set.detail},
                             {"documents", set.documents}});
  }
  return Json{{"ok", ok()}, {"sets", std::move(json_sets)}}.dump(2);
}

Result<MirrorReport> run_mirror(const Manifest& manifest, const MirrorOptions& options,
                                llm::HttpTransport& transport,
                                const std::string& only) {
  std::error_code ec;
  std::filesystem::create_directories(options.out_dir, ec);
  if (ec) {
    return fail("REFMIRROR_OUTPUT_FAILED",
                "cannot create output directory " + options.out_dir.string() + ": " +
                    ec.message());
  }

  MirrorReport report;
  for (const auto& set : manifest.sets) {
    if (!only.empty() && set.id != only) {
      continue;
    }
    SetReport entry;
    entry.id = set.id;
    entry.version = set.version;

    if (!set.generate.empty()) {
      if (set.generate != "doxygen") {
        entry.status = "unsupported";
        entry.detail = "unknown generator: " + set.generate;
        report.sets.push_back(std::move(entry));
        continue;
      }
      process::LocalProcessExecutor local_executor;
      process::ProcessExecutor& executor =
          options.executor != nullptr ? *options.executor : local_executor;
      auto generated = generate_doxygen(set, options, executor);
      if (!generated) {
        entry.status = "generation_failed";
        entry.detail = generated.error().message;
        report.sets.push_back(std::move(entry));
        continue;
      }
      bool injected = false;
      std::string injected_detail;
      for (const auto& document : *generated) {
        const auto findings = rag::scan_for_injected_instructions(document.text);
        if (!findings.empty()) {
          injected = true;
          injected_detail = findings.front().pattern + " in " + document.path;
          break;
        }
      }
      if (injected) {
        entry.status = "injection_found";
        entry.detail = "held for human review: " + injected_detail;
        report.sets.push_back(std::move(entry));
        continue;
      }
      const std::string stem = set.id + "-" + set.version;
      std::uint32_t number = 0;
      bool output_failed = false;
      for (const auto& document : *generated) {
        ++number;
        const auto path =
            options.out_dir / (stem + "-" + std::to_string(number) + ".md");
        std::ofstream out(path, std::ios::binary);
        if (!out) {
          entry.status = "output_failed";
          entry.detail = "cannot write " + path.string();
          output_failed = true;
          break;
        }
        out << front_matter(set, document.path) << document.text << "\n";
      }
      if (output_failed) {
        report.sets.push_back(std::move(entry));
        continue;
      }
      entry.status = "ok";
      entry.detail = "generated " + std::to_string(generated->size()) +
                     " documents";
      entry.documents = generated->size();
      report.sets.push_back(std::move(entry));
      continue;
    }

    auto content = fetch(set, options, transport);
    if (!content) {
      entry.status = "fetch_failed";
      entry.detail = content.error().message;
      report.sets.push_back(std::move(entry));
      continue;
    }

    const std::string actual_sha = cf::sha256_hex(*content);
    if (!set.sha256.empty() && actual_sha != set.sha256) {
      entry.status = "checksum_mismatch";
      entry.detail = "expected " + set.sha256 + ", got " + actual_sha;
      report.sets.push_back(std::move(entry));
      continue;
    }

    const std::string stem = set.id + "-" + set.version;

    if (!set.extract.empty()) {
      const std::string suffix = archive_suffix(set.source);
      if (suffix.empty()) {
        entry.status = "extract_failed";
        entry.detail = "cannot infer the archive format from " + set.source;
        report.sets.push_back(std::move(entry));
        continue;
      }
      TempDirectory temporary{options.out_dir / (".tmp-" + stem)};
      std::error_code error;
      std::filesystem::create_directories(temporary.path, error);
      if (error) {
        entry.status = "extract_failed";
        entry.detail = "cannot create temporary directory: " + error.message();
        report.sets.push_back(std::move(entry));
        continue;
      }
      const std::filesystem::path archive = temporary.path / ("source" + suffix);
      {
        std::ofstream raw(archive, std::ios::binary | std::ios::trunc);
        raw.write(content->data(), static_cast<std::streamsize>(content->size()));
        if (!raw) {
          entry.status = "extract_failed";
          entry.detail = "cannot write the downloaded archive";
          report.sets.push_back(std::move(entry));
          continue;
        }
      }
      const std::filesystem::path extracted = temporary.path / "extracted";
      std::filesystem::create_directories(extracted, error);
      process::LocalProcessExecutor local_executor;
      process::ProcessExecutor& executor =
          options.executor != nullptr ? *options.executor : local_executor;
      std::vector<std::string> argv;
      if (suffix == ".zip") {
        argv = {"unzip", "-q", archive.string(), "-d", extracted.string()};
      } else {
        std::string flag = "-xf";
        if (suffix == ".tar.gz" || suffix == ".tgz") flag = "-xzf";
        if (suffix == ".tar.bz2" || suffix == ".tbz2") flag = "-xjf";
        if (suffix == ".tar.xz" || suffix == ".txz") flag = "-xJf";
        argv = {"tar", flag, archive.string(), "-C", extracted.string()};
      }
      if (!run_extractor(executor, argv, extracted, options.timeout)) {
        entry.status = "extract_failed";
        entry.detail = "archive extraction failed for " + set.source;
        report.sets.push_back(std::move(entry));
        continue;
      }
      const auto files = extract_document_files(extracted, set.extract);
      if (files.empty()) {
        entry.status = "extract_failed";
        entry.detail = "no documents matched the extract patterns";
        report.sets.push_back(std::move(entry));
        continue;
      }

      struct Document {
        std::string path;
        std::string text;
      };
      std::vector<Document> documents;
      bool injected = false;
      std::string injected_detail;
      for (const auto& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
        const std::string cleaned =
            looks_like_html(raw) ? rag::clean_html_to_markdown(raw) : raw;
        const auto findings_in_doc = rag::scan_for_injected_instructions(cleaned);
        if (!findings_in_doc.empty()) {
          injected = true;
          injected_detail = findings_in_doc.front().pattern + " in " +
                            file.filename().string();
          break;
        }
        std::error_code relative_error;
        const auto relative =
            std::filesystem::relative(file, extracted, relative_error);
        documents.push_back(
            Document{relative_error ? file.filename().string()
                                    : relative.generic_string(),
                     cleaned});
      }
      if (injected) {
        entry.status = "injection_found";
        entry.detail = "held for human review: " + injected_detail;
        report.sets.push_back(std::move(entry));
        continue;
      }

      {
        std::ofstream raw(options.out_dir / (stem + ".archive"), std::ios::binary);
        if (raw) {
          raw.write(content->data(), static_cast<std::streamsize>(content->size()));
        }
      }
      std::uint32_t number = 0;
      for (const auto& document : documents) {
        ++number;
        const auto markdown_path =
            options.out_dir / (stem + "-" + std::to_string(number) + ".md");
        std::ofstream out(markdown_path, std::ios::binary);
        if (!out) {
          entry.status = "output_failed";
          entry.detail = "cannot write " + markdown_path.string();
          documents.clear();
          break;
        }
        out << front_matter(set, document.path) << document.text << "\n";
      }
      if (entry.status == "output_failed") {
        report.sets.push_back(std::move(entry));
        continue;
      }
      entry.status = "ok";
      entry.detail = "sha256 " + actual_sha + "; " +
                     std::to_string(documents.size()) + " extracted documents";
      entry.documents = documents.size();
      report.sets.push_back(std::move(entry));
      continue;
    }

    const std::string cleaned = looks_like_html(*content)
                                    ? rag::clean_html_to_markdown(*content)
                                    : *content;
    const auto findings = rag::scan_for_injected_instructions(cleaned);
    if (!findings.empty()) {
      entry.status = "injection_found";
      entry.detail = "held for human review: " + findings.front().pattern + " at line " +
                     std::to_string(findings.front().line) + " (" +
                     findings.front().snippet + ")";
      report.sets.push_back(std::move(entry));
      continue;
    }

    const auto markdown_path = options.out_dir / (stem + ".md");
    {
      std::ofstream out(markdown_path, std::ios::binary);
      if (!out) {
        entry.status = "output_failed";
        entry.detail = "cannot write " + markdown_path.string();
        report.sets.push_back(std::move(entry));
        continue;
      }
      out << front_matter(set) << cleaned << "\n";
    }
    {
      std::ofstream raw(options.out_dir / (stem + ".raw"), std::ios::binary);
      if (raw) {
        raw << *content;
      }
    }
    entry.status = "ok";
    entry.detail = "sha256 " + actual_sha;
    entry.documents = 1;
    report.sets.push_back(std::move(entry));
  }

  {
    std::ofstream out(options.out_dir / "report.json", std::ios::binary);
    if (out) {
      out << report.to_json() << "\n";
    }
  }
  return report;
}

}  // namespace cf::refmirror
