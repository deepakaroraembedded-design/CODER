// Unit tests for the OTLP/HTTP metrics export (design section 14).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/telemetry/metrics.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::telemetry::MetricKind;
using cf::telemetry::MetricPoint;
using cf::test::ScriptedHttpTransport;

std::vector<MetricPoint> sample_points() {
  return {
      MetricPoint{"cf_gate_pass_ratio", nlohmann::json{{"gate", "build"}}, 0.5,
                  MetricKind::Gauge},
      MetricPoint{"cf_llm_tokens_total",
                  nlohmann::json{{"role", "coder"},
                                 {"model", "m"},
                                 {"kind", "prompt"}},
                  42.0, MetricKind::Counter},
  };
}

TEST(TelemetryMetricsTest, BuildsTheOtlpJsonPayload) {
  const auto payload =
      cf::telemetry::build_otlp_metrics(sample_points(), "cf-test", 123456);
  ASSERT_TRUE(payload.contains("resourceMetrics"));
  const auto& resource = payload["resourceMetrics"][0]["resource"];
  EXPECT_EQ(resource["attributes"][0]["key"], "service.name");
  EXPECT_EQ(resource["attributes"][0]["value"]["stringValue"], "cf-test");

  const auto& metrics = payload["resourceMetrics"][0]["scopeMetrics"][0]["metrics"];
  ASSERT_EQ(metrics.size(), 2u);
  bool found_gate = false;
  bool found_tokens = false;
  for (const auto& metric : metrics) {
    if (metric["name"] == "cf_gate_pass_ratio") {
      EXPECT_TRUE(metric.contains("gauge"));
      const auto& point = metric["gauge"]["dataPoints"][0];
      EXPECT_DOUBLE_EQ(point["asDouble"].get<double>(), 0.5);
      EXPECT_EQ(point["timeUnixNano"], "123456");
      EXPECT_EQ(point["attributes"][0]["key"], "gate");
      EXPECT_EQ(point["attributes"][0]["value"]["stringValue"], "build");
      found_gate = true;
    }
    if (metric["name"] == "cf_llm_tokens_total") {
      EXPECT_TRUE(metric.contains("sum"));
      EXPECT_TRUE(metric["sum"]["isMonotonic"].get<bool>());
      EXPECT_EQ(metric["sum"]["dataPoints"][0]["asDouble"], 42.0);
      found_tokens = true;
    }
  }
  EXPECT_TRUE(found_gate);
  EXPECT_TRUE(found_tokens);
}

TEST(TelemetryMetricsTest, PostsToTheOtlpMetricsEndpoint) {
  ScriptedHttpTransport transport;
  transport.script(200, "{}");
  ASSERT_TRUE(cf::telemetry::export_otlp_metrics(
                  sample_points(), "http://collector:4318", transport)
                  .has_value());
  ASSERT_EQ(transport.requests().size(), 1u);
  EXPECT_EQ(transport.requests()[0].method, "POST");
  EXPECT_EQ(transport.requests()[0].url,
            "http://collector:4318/v1/metrics");
  EXPECT_NE(transport.requests()[0].body.find("resourceMetrics"),
            std::string::npos);

  ScriptedHttpTransport exact;
  exact.script(204, "");
  ASSERT_TRUE(cf::telemetry::export_otlp_metrics(
                  sample_points(), "http://collector:4318/v1/metrics/", exact)
                  .has_value());
  EXPECT_EQ(exact.requests()[0].url,
            "http://collector:4318/v1/metrics");
}

TEST(TelemetryMetricsTest, ReportsCollectorErrors) {
  ScriptedHttpTransport transport;
  transport.script(500, R"({"error":"boom"})");
  const auto exported = cf::telemetry::export_otlp_metrics(
      sample_points(), "http://collector:4318", transport);
  ASSERT_FALSE(exported.has_value());
  EXPECT_EQ(exported.error().code, "TELEMETRY_OTLP_FAILED");
}

}  // namespace
