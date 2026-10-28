// cf-refmirror: the offline reference documentation mirroring job
// (design section 6.3.1). Run as a scheduled CI job, outside any agent
// sandbox; only the URLs and repositories in the manifest are contacted.
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "cf/llm/client.hpp"
#include "cf/refmirror/manifest.hpp"
#include "cf/refmirror/mirror.hpp"
#include "cf/telemetry/logging.hpp"

int main(int argc, char** argv) {
  CLI::App app{"cf-refmirror: mirror the approved reference documentation sets"};
  std::string manifest_path;
  std::string out_dir;
  std::string only;
  app.add_option("--manifest", manifest_path, "Path to reference-docs/manifest.yaml")
      ->required();
  app.add_option("--out", out_dir, "Output directory for the cleaned mirror")
      ->required();
  app.add_option("--only", only, "Mirror a single set id");
  CLI11_PARSE(app, argc, argv);

  spdlog::set_default_logger(spdlog::stderr_color_mt("cf-refmirror"));
  cf::telemetry::configure_json_logging();

  auto manifest = cf::refmirror::load_manifest_file(manifest_path);
  if (!manifest) {
    spdlog::error("{}", manifest.error().message);
    return 2;
  }

  cf::refmirror::MirrorOptions options;
  options.out_dir = out_dir;
  cf::llm::CprTransport transport;
  auto report = cf::refmirror::run_mirror(*manifest, options, transport, only);
  if (!report) {
    spdlog::error("{}", report.error().message);
    return 2;
  }

  std::cout << report->to_json() << "\n";
  for (const auto& set : report->sets) {
    if (set.status == "ok") {
      spdlog::info("{}@{}: {}", set.id, set.version, set.status);
    } else {
      spdlog::warn("{}@{}: {} ({})", set.id, set.version, set.status, set.detail);
    }
  }
  return report->ok() ? 0 : 2;
}
