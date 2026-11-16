#include "cf/orch/report.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cf/llm/client.hpp"
#include "cf/orch/metrics.hpp"
#include "cf/orch/run_summary.hpp"
#include "cf/telemetry/metrics.hpp"

namespace cf::orch {
namespace {

using Json = nlohmann::json;

}  // namespace

Result<RunReport> write_run_report(store::RunStore& store,
                                   std::string_view run_id,
                                   const plan::Plan* plan,
                                   const std::filesystem::path& run_dir,
                                   const std::filesystem::path& out_dir) {
  auto summary = build_run_summary(store, run_id, plan, run_dir);
  if (!summary) {
    return tl::make_unexpected(summary.error());
  }
  (void)store.expire_waiver_requests(run_id, store::unix_now());
  auto metric_samples = collect_run_metrics(store, run_id, run_dir);
  if (!metric_samples) {
    return tl::make_unexpected(metric_samples.error());
  }

  Json metrics = Json::array();
  std::vector<telemetry::MetricPoint> metric_points;
  metric_points.reserve(metric_samples->size());
  for (const auto& sample : *metric_samples) {
    metrics.push_back(Json{{"name", sample.name},
                           {"labels", sample.labels},
                           {"value", sample.value}});
    const bool counter = sample.name == "cf_llm_tokens_total" ||
                         sample.name == "cf_llm_usd_total" ||
                         sample.name == "cf_escalations_total";
    metric_points.push_back(telemetry::MetricPoint{
        sample.name, sample.labels, sample.value,
        counter ? telemetry::MetricKind::Counter
                : telemetry::MetricKind::Gauge});
  }
  Json otlp = Json::object();
  const char* otlp_endpoint = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
  if (otlp_endpoint != nullptr && *otlp_endpoint != '\0') {
    llm::CprTransport transport;
    auto exported =
        telemetry::export_otlp_metrics(metric_points, otlp_endpoint, transport);
    if (exported) {
      otlp = Json{{"endpoint", otlp_endpoint}, {"exported", true}};
    } else {
      otlp = Json{{"endpoint", otlp_endpoint},
                  {"exported", false},
                  {"error", exported.error().message}};
      spdlog::warn("OTLP metrics export failed: {}", exported.error().message);
    }
  }

  Json conflicts = Json::array();
  auto conflict_rows = store.list_conflicts(run_id);
  if (conflict_rows) {
    for (const auto& row : *conflict_rows) {
      conflicts.push_back(Json{{"query", row.query},
                               {"topic", row.topic},
                               {"winner_chunk_id", row.winner_chunk_id},
                               {"overridden_chunk_ids", row.overridden_chunk_ids},
                               {"deciding_rule", row.deciding_rule},
                               {"unresolved", row.unresolved},
                               {"at", row.at}});
    }
  }

  Json escalations = Json::array();
  const std::filesystem::path escalation_dir = run_dir / "escalations";
  std::error_code ec;
  if (std::filesystem::is_directory(escalation_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(escalation_dir, ec)) {
      const auto path = entry.path() / "bundle.json";
      std::ifstream in(path);
      const Json bundle = Json::parse(in, nullptr, false);
      if (!bundle.is_discarded()) {
        escalations.push_back(Json{{"bundle", entry.path().filename().string()},
                                   {"reason", bundle.value("reason", "")}});
      }
    }
  }

  const Json document = Json{
      {"run", Json{{"id", summary->run.run_id},
                   {"state", summary->run.state},
                   {"plan_id", summary->run.plan_id},
                   {"created_at", summary->run.created_at},
                   {"cost",
                    Json{{"usd", summary->usd}, {"tokens", summary->tokens}}}}},
      {"milestones", summary->milestones},
      {"requirements", summary->requirements},
      {"defects", summary->defects},
      {"waivers", summary->waivers},
      {"waiver_requests", summary->waiver_requests},
      {"agent_usage",
       Json{{"by_role", summary->agent_usage_by_role},
            {"by_milestone", summary->agent_usage_by_milestone}}},
      {"conflicts", std::move(conflicts)},
      {"metrics", std::move(metrics)},
      {"otlp_metrics", std::move(otlp)},
      {"escalations", std::move(escalations)}};

  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    return fail("ORCH_REPORT_WRITE_FAILED",
                "cannot create the report directory: " + ec.message());
  }
  RunReport report;
  report.json_path = out_dir / "report.json";
  report.markdown_path = out_dir / "report.md";
  {
    std::ofstream json_out(report.json_path, std::ios::binary);
    if (!json_out) {
      return fail("ORCH_REPORT_WRITE_FAILED",
                  "cannot write " + report.json_path.string());
    }
    json_out << document.dump(2) << "\n";
  }
  {
    std::ofstream markdown(report.markdown_path, std::ios::binary);
    if (!markdown) {
      return fail("ORCH_REPORT_WRITE_FAILED",
                  "cannot write " + report.markdown_path.string());
    }
    markdown << "# Run report: " << summary->run.run_id << "\n\n";
    markdown << "- State: " << summary->run.state << "\n";
    markdown << "- Plan: " << summary->run.plan_id << "\n";
    markdown << "- Cost: $" << summary->usd << ", " << summary->tokens
             << " tokens\n\n";
    markdown << render_run_summary_markdown(*summary);
    markdown << "\n## Escalations\n\n";
    if (document["escalations"].empty()) {
      markdown << "None.\n";
    } else {
      for (const auto& row : document["escalations"]) {
        markdown << "- " << row["bundle"].get<std::string>() << ": "
                 << row["reason"].get<std::string>() << "\n";
      }
    }
  }
  return report;
}

}  // namespace cf::orch
