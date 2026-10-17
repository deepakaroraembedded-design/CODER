// cf-eval: the golden evaluation suite harness (design section 17.2).
//
//   cf-eval validate --suite eval/suite.yaml
//   cf-eval report   --suite eval/suite.yaml --outcomes outcomes.json [--out dir]
//   cf-eval bench    --suite eval/suite.yaml --benchmark eval/benchmarks/x.yaml
//                    --workspace bench-work --out bench-out --mcp build/cf-mcp
//
// `validate` checks the manifest and that every plan file parses; `report`
// scores recorded outcomes and writes eval-report.json / eval-report.md;
// `bench` runs candidate model configurations through the real run pipeline
// (design section 18.2 Phase 0b: model benchmark on the evaluation plans).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "cf/eval/benchmark.hpp"
#include "cf/eval/report.hpp"
#include "cf/eval/run_executor.hpp"
#include "cf/eval/score.hpp"
#include "cf/eval/suite.hpp"
#include "cf/rag/chunker.hpp"
#include "cf/rag/evaluation.hpp"
#include "cf/rag/loader.hpp"
#include "cf/rag/retriever.hpp"

namespace {

int run_validate(const std::string& suite_path) {
  auto manifest = cf::eval::load_suite_manifest(suite_path);
  if (!manifest) {
    std::cerr << "error: " << manifest.error().message << "\n";
    return 2;
  }
  auto plan_ids = cf::eval::validate_suite_plans(*manifest);
  if (!plan_ids) {
    std::cerr << "error: " << plan_ids.error().message << "\n";
    return 2;
  }
  std::map<std::string, int> by_category;
  std::map<std::string, int> by_language;
  std::map<std::string, int> by_tier;
  int held_out = 0;
  for (const auto& plan : manifest->plans) {
    ++by_category[cf::eval::to_string(plan.category)];
    if (!plan.language.empty()) {
      ++by_language[plan.language];
    }
    if (!plan.tier.empty()) {
      ++by_tier[plan.tier];
    }
    held_out += plan.held_out ? 1 : 0;
  }
  std::cout << "suite: " << manifest->suite_id << "\n";
  std::cout << "plans: " << manifest->plans.size() << " (held-out: " << held_out
            << ", repeats: " << manifest->repeats << ")\n";
  for (const auto& [name, count] : by_category) {
    std::cout << "  category " << name << ": " << count << "\n";
  }
  for (const auto& [name, count] : by_language) {
    std::cout << "  language " << name << ": " << count << "\n";
  }
  for (const auto& [name, count] : by_tier) {
    std::cout << "  tier " << name << ": " << count << "\n";
  }
  std::cout << "targets: completion >= " << manifest->targets.completion
            << ", security >= " << manifest->targets.security
            << ", recovery >= " << manifest->targets.recovery << "\n";
  return 0;
}

int run_report(const std::string& suite_path, const std::string& outcomes_path,
               const std::string& out_dir, bool strict) {
  auto manifest = cf::eval::load_suite_manifest(suite_path);
  if (!manifest) {
    std::cerr << "error: " << manifest.error().message << "\n";
    return 2;
  }
  auto outcomes = cf::eval::load_outcomes(outcomes_path);
  if (!outcomes) {
    std::cerr << "error: " << outcomes.error().message << "\n";
    return 2;
  }
  auto score = cf::eval::score_suite(*manifest, *outcomes);
  if (!score) {
    std::cerr << "error: " << score.error().message << "\n";
    return 2;
  }

  bool targets_met = true;
  for (const auto& target : score->targets) {
    if (target.applicable && !target.met) {
      targets_met = false;
    }
  }

  if (!out_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
      std::cerr << "error: cannot create " << out_dir << ": " << ec.message() << "\n";
      return 2;
    }
    const std::filesystem::path json_path =
        std::filesystem::path(out_dir) / "eval-report.json";
    const std::filesystem::path md_path =
        std::filesystem::path(out_dir) / "eval-report.md";
    std::ofstream json_out(json_path, std::ios::binary);
    json_out << cf::eval::score_to_json(*score);
    std::ofstream md_out(md_path, std::ios::binary);
    md_out << cf::eval::score_to_markdown(*score);
    std::cout << "wrote " << json_path.string() << " and " << md_path.string() << "\n";
  } else {
    std::cout << cf::eval::score_to_markdown(*score);
  }

  for (const auto& target : score->targets) {
    std::cout << "target " << target.name << ": ";
    if (!target.applicable) {
      std::cout << "n/a\n";
      continue;
    }
    std::cout << (target.met ? "met" : "MISSED") << " (" << target.observed << " vs "
              << target.target << ")\n";
  }
  if (strict && !targets_met) {
    return 1;
  }
  return 0;
}

int run_rag(const std::string& set_path, const std::string& docs_override, bool warn_only,
            bool list_chunks) {
  auto set = cf::rag::load_eval_set(set_path);
  if (!set) {
    std::cerr << "error: " << set.error().message << "\n";
    return 2;
  }
  const std::filesystem::path docs_dir =
      docs_override.empty() ? set->docs_dir : std::filesystem::path(docs_override);
  if (docs_dir.empty()) {
    std::cerr << "error: no docs directory (set 'docs' in the eval set or pass --docs)\n";
    return 2;
  }
  auto documents = cf::rag::load_document_dir(docs_dir);
  if (!documents) {
    std::cerr << "error: " << documents.error().message << "\n";
    return 2;
  }
  if (list_chunks) {
    for (const auto& document : *documents) {
      for (const auto& chunk : cf::rag::chunk_markdown(document.text)) {
        std::cout << document.doc_id << "@" << document.version << "#" << chunk.anchor
                  << "\n";
      }
    }
    return 0;
  }

  cf::rag::InMemoryRetriever retriever;
  for (auto& document : *documents) {
    auto added = retriever.add_document(std::move(document));
    if (!added) {
      std::cerr << "error: " << added.error().message << "\n";
      return 2;
    }
  }
  auto metrics = cf::rag::evaluate_retrieval(retriever, *set);
  if (!metrics) {
    std::cerr << "error: " << metrics.error().message << "\n";
    return 2;
  }
  std::cout << "collection: " << metrics->collection << "\n";
  std::cout << "queries: " << metrics->queries << "  k: " << metrics->k << "\n";
  std::cout << "recall@" << metrics->k << ": " << metrics->recall << " (target >= "
            << cf::rag::kRecallTarget << ")\n";
  std::cout << "mrr: " << metrics->mrr << " (target >= " << cf::rag::kMrrTarget << ")\n";
  std::cout << "misses: " << metrics->misses << "\n";
  const bool met = cf::rag::meets_retrieval_targets(*metrics);
  std::cout << (met ? "targets met" : "TARGETS MISSED") << "\n";
  if (!met && !warn_only) {
    return 1;
  }
  return 0;
}

struct BenchOptions {
  std::string suite_path;
  std::string benchmark_path;
  std::string workspace;
  std::string out_dir;
  std::string profiles_dir = "profiles";
  std::string opencode_binary = "opencode";
  std::string mcp_binary;
  std::string docs_dir;
  std::string sandbox_image;
  std::vector<std::string> candidates;  // empty: every candidate
  std::vector<std::string> plans;       // empty: the benchmark's plan list
  bool dry_run = false;
  bool drop_scratch = false;
};

int run_bench(const BenchOptions& options) {
  auto manifest = cf::eval::load_suite_manifest(options.suite_path);
  if (!manifest) {
    std::cerr << "error: " << manifest.error().message << "\n";
    return 2;
  }
  auto config = cf::eval::load_benchmark_config(options.benchmark_path);
  if (!config) {
    std::cerr << "error: " << config.error().message << "\n";
    return 2;
  }
  auto restricted = cf::eval::restrict_suite(*manifest, *config);
  if (!restricted) {
    std::cerr << "error: " << restricted.error().message << "\n";
    return 2;
  }
  if (!options.plans.empty()) {
    std::vector<cf::eval::PlanEntry> filtered;
    for (const auto& plan : restricted->plans) {
      if (std::find(options.plans.begin(), options.plans.end(), plan.id) !=
          options.plans.end()) {
        filtered.push_back(plan);
      }
    }
    if (filtered.empty()) {
      std::cerr << "error: no plan matches the --plan filter\n";
      return 2;
    }
    restricted->plans = std::move(filtered);
  }
  if (options.mcp_binary.empty() && !options.dry_run) {
    std::cerr << "error: --mcp is required to run the benchmark\n";
    return 2;
  }
  if (options.workspace.empty() && !options.dry_run) {
    std::cerr << "error: --workspace is required to run the benchmark\n";
    return 2;
  }

  std::vector<cf::eval::Candidate> selected;
  for (const auto& candidate : config->candidates) {
    if (options.candidates.empty() ||
        std::find(options.candidates.begin(), options.candidates.end(),
                  candidate.name) != options.candidates.end()) {
      selected.push_back(candidate);
    }
  }
  if (selected.empty()) {
    std::cerr << "error: no candidate matches the --candidate filter\n";
    return 2;
  }

  std::cout << "benchmark: " << config->benchmark_id << "\n";
  std::cout << "plans: " << restricted->plans.size()
            << "  repeats: " << restricted->repeats << "\n";
  for (const auto& candidate : selected) {
    std::cout << "candidate " << candidate.name << ":";
    for (const auto& [role, model] : candidate.role_models) {
      std::cout << " " << role << "=" << model;
    }
    std::cout << "\n";
  }
  if (options.dry_run) {
    return 0;
  }

  cf::process::LocalProcessExecutor executor;
  std::vector<cf::eval::CandidateResult> results;
  for (const auto& candidate : selected) {
    cf::eval::RunExecutorOptions executor_options;
    executor_options.workspace_root = options.workspace;
    executor_options.suite_dir = manifest->base_dir;
    executor_options.profiles_dir = options.profiles_dir;
    executor_options.hidden_dir = manifest->base_dir / "hidden";
    executor_options.opencode_binary = options.opencode_binary;
    executor_options.mcp_binary = options.mcp_binary;
    executor_options.docs_dir = options.docs_dir;
    executor_options.sandbox_image = options.sandbox_image;
    executor_options.provider_base_url = config->provider_base_url;
    executor_options.api_key_env = config->api_key_env;
    executor_options.role_models = candidate.role_models;
    executor_options.role_options = candidate.role_options;
    executor_options.prices = config->prices;
    executor_options.keep_scratch = !options.drop_scratch;
    cf::eval::RunProcessExecutor plan_executor(std::move(executor_options), executor);

    cf::eval::CandidateResult result;
    result.name = candidate.name;
    auto score = cf::eval::run_suite(*restricted, plan_executor, &result.outcomes);
    if (!score) {
      std::cerr << "error: candidate " << candidate.name << ": "
                << score.error().message << "\n";
      return 1;
    }
    // Re-score with the candidate recorded as the configuration.
    result.outcomes.config = candidate.name;
    auto rescored = cf::eval::score_suite(*restricted, result.outcomes);
    if (!rescored) {
      std::cerr << "error: " << rescored.error().message << "\n";
      return 1;
    }
    result.score = *rescored;
    results.push_back(std::move(result));

    if (!options.out_dir.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(options.out_dir, ec);
      const std::filesystem::path prefix =
          std::filesystem::path(options.out_dir) / candidate.name;
      std::ofstream outcomes_out(prefix.string() + "-outcomes.json", std::ios::binary);
      outcomes_out << cf::eval::outcomes_to_json(results.back().outcomes);
      std::ofstream json_out(prefix.string() + "-report.json", std::ios::binary);
      json_out << cf::eval::score_to_json(results.back().score);
      std::ofstream md_out(prefix.string() + "-report.md", std::ios::binary);
      md_out << cf::eval::score_to_markdown(results.back().score);
    }
  }

  const auto winner = cf::eval::recommend_candidate(results);
  std::cout << cf::eval::benchmark_summary_to_markdown(*config, results, winner);
  if (!options.out_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options.out_dir, ec);
    std::ofstream json_out(std::filesystem::path(options.out_dir) / "benchmark.json",
                           std::ios::binary);
    json_out << cf::eval::benchmark_summary_to_json(*config, results, winner);
    std::ofstream md_out(std::filesystem::path(options.out_dir) / "benchmark.md",
                         std::ios::binary);
    md_out << cf::eval::benchmark_summary_to_markdown(*config, results, winner);
    std::cout << "wrote " << (std::filesystem::path(options.out_dir) / "benchmark.json")
                     .string()
              << " and "
              << (std::filesystem::path(options.out_dir) / "benchmark.md").string()
              << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"cf-eval: golden evaluation suite harness (design section 17.2)"};

  std::string suite_path;
  std::string outcomes_path;
  std::string out_dir;
  bool strict = false;

  auto* validate = app.add_subcommand("validate", "Validate a suite manifest and its plans");
  validate->add_option("--suite", suite_path, "Path to the suite manifest")->required();

  auto* report = app.add_subcommand("report", "Score recorded outcomes into a report");
  report->add_option("--suite", suite_path, "Path to the suite manifest")->required();
  report->add_option("--outcomes", outcomes_path, "Path to the outcomes JSON")->required();
  report->add_option("--out", out_dir, "Write eval-report.json/.md into this directory");
  report->add_flag("--strict", strict, "Exit 1 when a target is missed");

  std::string rag_set;
  std::string rag_docs;
  bool rag_warn_only = false;
  bool rag_list_chunks = false;
  auto* rag = app.add_subcommand("rag", "Evaluate retrieval against a labeled set");
  rag->add_option("--set", rag_set, "Path to the RAG evaluation set")->required();
  rag->add_option("--docs", rag_docs, "Override the docs directory from the set");
  rag->add_flag("--warn-only", rag_warn_only, "Do not fail below the recall/MRR targets");
  rag->add_flag("--list-chunks", rag_list_chunks, "Print chunk ids and exit (labeling aid)");

  BenchOptions bench;
  auto* bench_command =
      app.add_subcommand("bench", "Run candidate models on the golden suite (Phase 0b)");
  bench_command->add_option("--suite", bench.suite_path, "Path to the suite manifest")
      ->required();
  bench_command
      ->add_option("--benchmark", bench.benchmark_path, "Path to the benchmark config")
      ->required();
  bench_command->add_option("--workspace", bench.workspace,
                            "Scratch workspace for the plan runs");
  bench_command->add_option("--out", bench.out_dir,
                            "Write outcomes and reports into this directory");
  bench_command->add_option("--profiles", bench.profiles_dir, "Toolchain profiles");
  bench_command->add_option("--opencode", bench.opencode_binary, "opencode binary");
  bench_command->add_option("--mcp", bench.mcp_binary, "cf-mcp binary");
  bench_command->add_option("--docs", bench.docs_dir, "Ingested company documents");
  bench_command->add_option("--sandbox-image", bench.sandbox_image,
                            "Run agent servers in this container image");
  bench_command->add_option("--candidate", bench.candidates,
                            "Only run this candidate (repeatable)");
  bench_command->add_option("--plan", bench.plans,
                            "Only run this plan (repeatable)");
  bench_command->add_flag("--dry-run", bench.dry_run,
                          "Validate the configuration and print the plan");
  bench_command->add_flag("--drop-scratch", bench.drop_scratch,
                          "Delete each plan's scratch directory after it runs");

  CLI11_PARSE(app, argc, argv);
  if (*validate) {
    return run_validate(suite_path);
  }
  if (*report) {
    return run_report(suite_path, outcomes_path, out_dir, strict);
  }
  if (*rag) {
    return run_rag(rag_set, rag_docs, rag_warn_only, rag_list_chunks);
  }
  if (*bench_command) {
    return run_bench(bench);
  }
  std::cerr << app.help() << "\n";
  return 2;
}
