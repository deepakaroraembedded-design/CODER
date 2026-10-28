// cf-telemetry: OTLP/HTTP metrics export (design section 14).
//
// The metric sample names and labels follow the design. The payload is the
// OpenTelemetry OTLP/JSON encoding, POSTed to
// $OTEL_EXPORTER_OTLP_ENDPOINT/v1/metrics (or the exact /v1/metrics URL).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
#include "cf/llm/client.hpp"

namespace cf::telemetry {

enum class MetricKind { Gauge, Counter };

struct MetricPoint {
  std::string name;
  nlohmann::json labels = nlohmann::json::object();
  double value = 0.0;
  MetricKind kind = MetricKind::Gauge;
};

// Builds the OTLP/JSON request body. `time_unix_nano` is shared by every
// data point so one snapshot is consistent.
nlohmann::json build_otlp_metrics(
    const std::vector<MetricPoint>& points, std::string_view service_name,
    std::int64_t time_unix_nano);

// POSTs the snapshot to the OTLP/HTTP endpoint. A 2xx response is success.
Status export_otlp_metrics(const std::vector<MetricPoint>& points,
                           std::string_view endpoint,
                           llm::HttpTransport& transport,
                           std::string_view service_name = "coder-framework");

}  // namespace cf::telemetry
