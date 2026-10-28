#include "cf/orch/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace cf::orch {
namespace {

using Json = nlohmann::json;

struct UsageTotals {
  double usd = 0.0;
  std::int64_t prompt_tokens = 0;
  std::int64_t completion_tokens = 0;
  std::int64_t cached_tokens = 0;
};

std::int64_t number_or_zero(const Json& object, const char* key) {
  const auto value = object.find(key);
  return value != object.end() && value->is_number()
             ? static_cast<std::int64_t>(value->get<double>())
             : 0;
}

UsageTotals usage_totals(const Json& usage) {
  UsageTotals totals;
  if (!usage.is_object()) {
    return totals;
  }
  const auto cost = usage.find("cost");
  if (cost != usage.end() && cost->is_number()) {
    totals.usd = cost->get<double>();
  }
  const auto tokens = usage.find("tokens");
  if (tokens != usage.end() && tokens->is_object()) {
    totals.prompt_tokens = number_or_zero(*tokens, "input") +
                           number_or_zero(*tokens, "prompt");
    totals.completion_tokens = number_or_zero(*tokens, "output") +
                               number_or_zero(*tokens, "completion") +
                               number_or_zero(*tokens, "reasoning");
    const auto cache = tokens->find("cache");
    if (cache != tokens->end() && cache->is_object()) {
      totals.cached_tokens = number_or_zero(*cache, "read") +
                             number_or_zero(*cache, "write");
    }
  }
  return totals;
}

void read_agent_usage(const std::filesystem::path& run_dir,
                      std::map<std::string, UsageTotals, std::less<>>& by_role,
                      std::map<std::pair<std::string, std::string>, UsageTotals,
                               std::less<>>& by_role_model) {
  std::error_code ec;
  if (run_dir.empty() || !std::filesystem::is_directory(run_dir, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(run_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("usage-", 0) != 0 || entry.path().extension() != ".jsonl") {
      continue;
    }
    const std::string file_role = name.substr(6, name.size() - 6 - 6);
    std::ifstream in(entry.path(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const Json parsed = Json::parse(line, nullptr, false);
      if (!parsed.is_object()) {
        continue;
      }
      const std::string role = parsed.value("role", file_role);
      const Json usage = parsed.value("usage", Json::object());
      std::string model = "unknown";
      if (const auto model_node = usage.find("model");
          model_node != usage.end() && model_node->is_object()) {
        model = model_node->value("id", "unknown");
      }
      const UsageTotals totals = usage_totals(usage);
      by_role[role].usd += totals.usd;
      by_role[role].prompt_tokens += totals.prompt_tokens;
      by_role[role].completion_tokens += totals.completion_tokens;
      by_role[role].cached_tokens += totals.cached_tokens;
      auto& combined = by_role_model[{role, model}];
      combined.usd += totals.usd;
      combined.prompt_tokens += totals.prompt_tokens;
      combined.completion_tokens += totals.completion_tokens;
      combined.cached_tokens += totals.cached_tokens;
    }
  }
}

}  // namespace

Result<std::vector<MetricSample>> collect_run_metrics(
    store::RunStore& store, std::string_view run_id,
    const std::filesystem::path& run_dir) {
  std::vector<MetricSample> samples;

  auto gates = store.list_all_gate_results(run_id);
  auto milestones = store.list_milestones(run_id);
  auto defects = store.list_defects(run_id);
  auto calls = store.list_llm_calls(run_id);
  auto transitions = store.list_transitions(run_id);
  auto rag_latencies = store.list_rag_latencies(run_id);
  if (!gates || !milestones || !defects || !calls || !transitions ||
      !rag_latencies) {
    return fail("ORCH_METRICS_READ_FAILED", "cannot read the run store");
  }

  std::map<std::string, std::pair<int, int>, std::less<>> gate_counts;
  for (const auto& row : *gates) {
    auto& counts = gate_counts[row.gate];
    ++counts.second;
    if (row.passed) {
      ++counts.first;
    }
  }
  for (const auto& [gate, counts] : gate_counts) {
    if (counts.second == 0) {
      continue;
    }
    samples.push_back(MetricSample{"cf_gate_pass_ratio",
                                   Json{{"gate", gate}},
                                   static_cast<double>(counts.first) /
                                       static_cast<double>(counts.second)});
  }

  for (const auto& milestone : *milestones) {
    samples.push_back(MetricSample{"cf_iterations",
                                   Json{{"milestone", milestone.milestone_id}},
                                   static_cast<double>(milestone.iterations)});
  }

  std::size_t open_defects = 0;
  for (const auto& defect : *defects) {
    if (defect.status != "VERIFIED" && defect.status != "WONT_FIX") {
      ++open_defects;
    }
  }
  samples.push_back(
      MetricSample{"cf_defects_open", Json::object(),
                   static_cast<double>(open_defects)});

  std::map<std::string, UsageTotals, std::less<>> usd_by_role;
  std::map<std::pair<std::string, std::string>, UsageTotals, std::less<>>
      tokens_by_role_model;
  for (const auto& call : *calls) {
    const std::string model = call.model.empty() ? "unknown" : call.model;
    usd_by_role[call.role].usd += call.usd;
    auto& totals = tokens_by_role_model[{call.role, model}];
    totals.prompt_tokens += call.prompt_tokens;
    totals.completion_tokens += call.completion_tokens;
    totals.cached_tokens += call.cached_tokens;
  }
  read_agent_usage(run_dir, usd_by_role, tokens_by_role_model);
  for (const auto& [key, totals] : tokens_by_role_model) {
    const auto& [role, model] = key;
    samples.push_back(MetricSample{
        "cf_llm_tokens_total",
        Json{{"role", role}, {"model", model}, {"kind", "prompt"}},
        static_cast<double>(totals.prompt_tokens)});
    samples.push_back(MetricSample{
        "cf_llm_tokens_total",
        Json{{"role", role}, {"model", model}, {"kind", "completion"}},
        static_cast<double>(totals.completion_tokens)});
    samples.push_back(MetricSample{
        "cf_llm_tokens_total",
        Json{{"role", role}, {"model", model}, {"kind", "cached"}},
        static_cast<double>(totals.cached_tokens)});
  }
  for (const auto& [role, totals] : usd_by_role) {
    samples.push_back(MetricSample{"cf_llm_usd_total", Json{{"role", role}},
                                   totals.usd});
  }

  std::map<std::string, std::size_t, std::less<>> escalations;
  for (const auto& transition : *transitions) {
    if (transition.to_state != "Escalated") {
      continue;
    }
    const Json event = Json::parse(transition.event_json, nullptr, false);
    std::string reason = event.is_object() ? event.value("event", "unknown")
                                            : "unknown";
    ++escalations[reason.empty() ? "unknown" : reason];
  }
  for (const auto& [reason, count] : escalations) {
    samples.push_back(MetricSample{"cf_escalations_total",
                                   Json{{"reason", reason}},
                                   static_cast<double>(count)});
  }

  if (!rag_latencies->empty()) {
    std::vector<std::int64_t> values;
    values.reserve(rag_latencies->size());
    std::int64_t sum = 0;
    for (const auto& row : *rag_latencies) {
      values.push_back(row.latency_ms);
      sum += row.latency_ms;
    }
    std::sort(values.begin(), values.end());
    samples.push_back(MetricSample{
        "cf_rag_latency_ms", Json{{"quantile", "avg"}},
        static_cast<double>(sum) / static_cast<double>(values.size())});
    const std::size_t index =
        static_cast<std::size_t>(std::ceil(0.95 * values.size())) - 1;
    samples.push_back(MetricSample{
        "cf_rag_latency_ms", Json{{"quantile", "p95"}},
        static_cast<double>(values[std::min(index, values.size() - 1)])});
  }

  return samples;
}

}  // namespace cf::orch
