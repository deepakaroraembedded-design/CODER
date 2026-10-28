#include "cf/telemetry/metrics.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cf::telemetry {
namespace {

using Json = nlohmann::json;

Json attributes(const Json& labels) {
  Json out = Json::array();
  if (!labels.is_object()) {
    return out;
  }
  for (const auto& [key, value] : labels.items()) {
    out.push_back(Json{{"key", key},
                       {"value",
                        Json{{"stringValue",
                              value.is_string() ? value.get<std::string>()
                                                : value.dump()}}}});
  }
  return out;
}

}  // namespace

Json build_otlp_metrics(const std::vector<MetricPoint>& points,
                        std::string_view service_name,
                        std::int64_t time_unix_nano) {
  const std::string time = std::to_string(time_unix_nano);
  std::map<std::string, std::pair<MetricKind, Json>, std::less<>> grouped;
  for (const auto& point : points) {
    auto& entry = grouped[point.name];
    entry.first = point.kind;
    if (entry.second.is_null()) {
      entry.second = Json::array();
    }
    entry.second.push_back(Json{
        {"attributes", attributes(point.labels)},
        {"timeUnixNano", time},
        {"asDouble", point.value},
    });
  }

  Json metrics = Json::array();
  for (const auto& [name, entry] : grouped) {
    Json metric = Json{{"name", name}};
    if (entry.first == MetricKind::Counter) {
      metric["sum"] = Json{{"dataPoints", entry.second},
                           {"aggregationTemporality", 2},
                           {"isMonotonic", true}};
    } else {
      metric["gauge"] = Json{{"dataPoints", entry.second}};
    }
    metrics.push_back(std::move(metric));
  }

  Json resource_attributes = Json::array();
  resource_attributes.push_back(Json{
      {"key", "service.name"},
      {"value", Json{{"stringValue", std::string(service_name)}}}});
  Json scope_metrics = Json::array();
  scope_metrics.push_back(Json{{"scope", Json{{"name", "cf"}}},
                               {"metrics", std::move(metrics)}});
  Json resource_metrics = Json::array();
  resource_metrics.push_back(Json{
      {"resource", Json{{"attributes", std::move(resource_attributes)}}},
      {"scopeMetrics", std::move(scope_metrics)}});
  return Json{{"resourceMetrics", std::move(resource_metrics)}};

}

Status export_otlp_metrics(const std::vector<MetricPoint>& points,
                           std::string_view endpoint,
                           llm::HttpTransport& transport,
                           std::string_view service_name) {
  if (endpoint.empty()) {
    return fail("TELEMETRY_OTLP_ENDPOINT", "the OTLP endpoint is empty");
  }
  std::string url(endpoint);
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  constexpr std::string_view kMetricsPath = "/v1/metrics";
  if (url.size() < kMetricsPath.size() ||
      url.compare(url.size() - kMetricsPath.size(), kMetricsPath.size(),
                  kMetricsPath) != 0) {
    url += kMetricsPath;
  }

  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  llm::HttpRequest request;
  request.url = url;
  request.headers = {{"Content-Type", "application/json"},
                     {"Accept", "application/json"}};
  request.body = build_otlp_metrics(points, service_name, now).dump();
  request.timeout = std::chrono::seconds(30);
  auto response = transport.post(request);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  if (response->status < 200 || response->status >= 300) {
    return fail("TELEMETRY_OTLP_FAILED",
                "OTLP endpoint returned HTTP " +
                    std::to_string(response->status));
  }
  return {};
}

}  // namespace cf::telemetry
