// Unit tests for trusted step process execution (design section 6.7.3).
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "cf/common/process.hpp"

namespace {

using cf::process::LocalProcessExecutor;
using cf::process::ProcessRequest;

TEST(ProcessTest, EchoCapturesStdout) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"echo", "hello"};
  const auto result = executor.run(request);
  EXPECT_TRUE(result.launched) << result.error;
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_FALSE(result.timed_out);
  EXPECT_EQ(result.stdout_text, "hello\n");
}

TEST(ProcessTest, NonZeroExitIsReported) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"sh", "-c", "exit 3"};
  const auto result = executor.run(request);
  EXPECT_TRUE(result.launched);
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_FALSE(result.timed_out);
}

TEST(ProcessTest, MissingProgramIsALaunchFailure) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"cf-definitely-not-a-program-xyz"};
  const auto result = executor.run(request);
  EXPECT_FALSE(result.launched);
  EXPECT_FALSE(result.error.empty());
}

TEST(ProcessTest, EmptyArgvIsRejected) {
  LocalProcessExecutor executor;
  const auto result = executor.run(ProcessRequest{});
  EXPECT_FALSE(result.launched);
}

TEST(ProcessTest, TimeoutKillsTheProcess) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"sleep", "30"};
  request.timeout = std::chrono::milliseconds(150);
  const auto result = executor.run(request);
  EXPECT_TRUE(result.launched);
  EXPECT_TRUE(result.timed_out);
  EXPECT_LT(result.duration_ms, 5000);
}

TEST(ProcessTest, StderrIsCapturedSeparately) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"sh", "-c", "echo oops >&2; exit 1"};
  const auto result = executor.run(request);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.stderr_text.find("oops"), std::string::npos);
  EXPECT_TRUE(result.stdout_text.empty());
}

TEST(ProcessTest, EnvironmentVariablesArePassed) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"sh", "-c", "printf %s \"$CF_TEST_VAR\""};
  request.environment = {{"CF_TEST_VAR", "abc"}};
  const auto result = executor.run(request);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text, "abc");
}

TEST(ProcessTest, WorkingDirectoryIsUsed) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"pwd"};
  request.working_directory = "/tmp";
  const auto result = executor.run(request);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.stdout_text.find("/tmp"), std::string::npos);
}

TEST(ProcessTest, OutputIsTruncatedAtTheLimit) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"sh", "-c", "yes hello | head -c 200000"};
  request.output_limit_bytes = 1000;
  const auto result = executor.run(request);
  EXPECT_TRUE(result.launched);
  EXPECT_TRUE(result.truncated_stdout);
  EXPECT_LE(result.stdout_text.size(), 1000u);
}

TEST(ProcessTest, SignalDeathReportsConventionalExitCode) {
  LocalProcessExecutor executor;
  ProcessRequest request;
  request.argv = {"sh", "-c", "kill -TERM $$"};
  const auto result = executor.run(request);
  EXPECT_TRUE(result.launched);
  EXPECT_EQ(result.exit_code, 128 + 15);
}

}  // namespace
