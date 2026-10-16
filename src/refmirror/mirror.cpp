#include "cf/refmirror/mirror.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
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

std::string front_matter(const ReferenceSet& set) {
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
      entry.status = "unsupported";
      entry.detail = "generator not implemented yet: " + set.generate;
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

    const std::string stem = set.id + "-" + set.version;
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
