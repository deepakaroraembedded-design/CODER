// cf-eval: model benchmark configuration (design sections 17.2, 18.2 Phase
// 0b).
//
// A benchmark lists candidate model configurations (role -> exact model id)
// for the golden suite. Each candidate is run against the suite's plans
// through the real run pipeline; the scored outcomes decide which exact model
// versions are pinned (design D11: benchmark-only selection with no
// defaults).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/eval/outcome.hpp"
#include "cf/eval/score.hpp"
#include "cf/eval/suite.hpp"

namespace cf::eval {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kBenchmarkParseFailed = "EVAL_BENCHMARK_PARSE_FAILED";
inline constexpr const char* kBenchmarkInvalid = "EVAL_BENCHMARK_INVALID";
}  // namespace error_codes

// Price per million tokens, used to estimate cost when the provider or the
// agent harness does not report it.
struct ModelPrice {
  double usd_per_mtok_in = 0.0;
  double usd_per_mtok_out = 0.0;
};

struct Candidate {
  std::string name;
  std::map<std::string, std::string, std::less<>> role_models;  // coder|reviewer|qa
};

struct BenchmarkConfig {
  std::string benchmark_id;
  std::string provider_base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key_env = "FIREWORKS_API_KEY";
  // 0: use the suite's repeat count.
  int repeats = 0;
  // Plan ids to run; empty runs every plan in the suite.
  std::vector<std::string> plans;
  std::vector<Candidate> candidates;
  // Optional per-model pricing for cost estimation.
  std::map<std::string, ModelPrice, std::less<>> prices;
  std::filesystem::path base_dir;
};

Result<BenchmarkConfig> parse_benchmark_config(std::string_view yaml,
                                               const std::filesystem::path& base_dir = {});
Result<BenchmarkConfig> load_benchmark_config(const std::filesystem::path& path);

// Restricts the manifest to the benchmark's plans (manifest order) and
// applies the benchmark's repeat count. An unknown plan id is an error.
Result<SuiteManifest> restrict_suite(const SuiteManifest& manifest,
                                     const BenchmarkConfig& benchmark);

// The scored outcome of one candidate.
struct CandidateResult {
  std::string name;
  SuiteScore score;
  OutcomeSet outcomes;
};

// Ranks candidates for the benchmark report: run completion first, then
// milestone completion, then hidden-test pass rate, then lower mean cost per
// milestone. Ties keep the earlier candidate. Returns the index into
// `results`, or nullopt when the list is empty.
std::optional<std::size_t> recommend_candidate(
    const std::vector<CandidateResult>& results);

std::string benchmark_summary_to_json(const BenchmarkConfig& config,
                                      const std::vector<CandidateResult>& results,
                                      std::optional<std::size_t> winner);
std::string benchmark_summary_to_markdown(const BenchmarkConfig& config,
                                          const std::vector<CandidateResult>& results,
                                          std::optional<std::size_t> winner);

}  // namespace cf::eval
