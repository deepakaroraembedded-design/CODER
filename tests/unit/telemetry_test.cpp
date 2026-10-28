// Unit tests for the structured logging context and JSON formatter
// (design sections 6.11, 14).
#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "cf/telemetry/logging.hpp"

namespace {

TEST(TelemetryTest, JsonLineCarriesTheRunContext) {
  std::ostringstream captured;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
  auto logger = std::make_shared<spdlog::logger>("telemetry-test", sink);
  logger->set_level(spdlog::level::info);
  cf::telemetry::install_json_formatter(logger);

  {
    cf::telemetry::ScopedContext context("R12", "M3", "coder", "trace-123");
    logger->info("hello {}", "world");
  }

  const std::string text = captured.str();
  ASSERT_FALSE(text.empty());
  const std::size_t newline = text.find('\n');
  ASSERT_NE(newline, std::string::npos);
  const auto event = nlohmann::json::parse(text.substr(0, newline));
  EXPECT_EQ(event["level"], "info");
  EXPECT_EQ(event["logger"], "telemetry-test");
  EXPECT_EQ(event["message"], "hello world");
  EXPECT_EQ(event["run_id"], "R12");
  EXPECT_EQ(event["milestone_id"], "M3");
  EXPECT_EQ(event["agent"], "coder");
  EXPECT_EQ(event["trace_id"], "trace-123");
  EXPECT_FALSE(event["time"].get<std::string>().empty());
}

TEST(TelemetryTest, ScopedContextRestoresThePreviousContext) {
  {
    cf::telemetry::ScopedContext outer("R12", "M1", "coder");
    EXPECT_EQ(cf::telemetry::current_context().run_id, "R12");
    EXPECT_EQ(cf::telemetry::current_context().milestone_id, "M1");
    EXPECT_FALSE(cf::telemetry::current_context().trace_id.empty());
    {
      cf::telemetry::ScopedContext inner("R12", "M2", "reviewer", "trace-2");
      EXPECT_EQ(cf::telemetry::current_context().milestone_id, "M2");
      EXPECT_EQ(cf::telemetry::current_context().agent, "reviewer");
      EXPECT_EQ(cf::telemetry::current_context().trace_id, "trace-2");
    }
    EXPECT_EQ(cf::telemetry::current_context().milestone_id, "M1");
    EXPECT_EQ(cf::telemetry::current_context().agent, "coder");
  }
  EXPECT_TRUE(cf::telemetry::current_context().run_id.empty());
  EXPECT_TRUE(cf::telemetry::current_context().trace_id.empty());
}

TEST(TelemetryTest, TraceIdsAreFresh) {
  const std::string first = cf::telemetry::new_trace_id();
  const std::string second = cf::telemetry::new_trace_id();
  EXPECT_EQ(first.size(), 16u);
  EXPECT_EQ(second.size(), 16u);
  EXPECT_NE(first, second);
}

}  // namespace
