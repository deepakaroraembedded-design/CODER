// Unit tests for replaying archived LLM calls (design sections 6.11, 14).
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/cli/commands.hpp"
#include "cf/llm/client.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_replay_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class EnvGuard {
 public:
  EnvGuard(std::string name, std::string value) : name_(std::move(name)) {
    setenv(name_.c_str(), value.c_str(), 1);
  }
  ~EnvGuard() { unsetenv(name_.c_str()); }

 private:
  std::string name_;
};

TEST(ReplayTest, ResendsTheArchivedRequestWithAModelOverride) {
  EnvGuard archive_key(
      "CF_ARCHIVE_KEY",
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  EnvGuard api_key("CF_REPLAY_TEST_KEY", "test-key");

  const auto dir = make_temp_dir();
  const auto run_dir = cf::cli::run_directory(dir, "R12");
  std::filesystem::create_directories(run_dir / "llm");

  cf::llm::ClientOptions options;
  options.base_url = "https://api.fireworks.test/v1";
  options.api_key = "test-key";
  options.archive_dir = run_dir / "llm";
  auto provider = std::make_unique<ScriptedHttpTransport>();
  provider->script(200,
                   R"({"choices":[{"message":{"role":"assistant","content":"first"}}],
                       "usage":{"prompt_tokens":10,"completion_tokens":2}})");
  cf::llm::FireworksClient client(options, std::move(provider));

  cf::llm::ChatRequest request;
  request.model = "original-model";
  request.messages.push_back(
      cf::llm::ChatMessage{"user", "replay me", nlohmann::json(), "", ""});
  request.context = {"R12", "planner", "M1", ""};
  auto first = client.chat(request);
  ASSERT_TRUE(first.has_value()) << first.error().message;

  std::string call_id;
  for (const auto& entry : std::filesystem::directory_iterator(run_dir / "llm")) {
    if (entry.path().extension() == ".cfar") {
      call_id = entry.path().stem().string();
    }
  }
  ASSERT_FALSE(call_id.empty());

  ScriptedHttpTransport replay_transport;
  replay_transport.script(200,
                          R"({"choices":[{"message":{"role":"assistant","content":"replayed"}}]})");
  cf::cli::ReplayOptions replay;
  replay.workspace = dir;
  replay.run_id = "R12";
  replay.call_id = call_id;
  replay.model = "replay-model";
  replay.api_key_env = "CF_REPLAY_TEST_KEY";
  replay.transport = &replay_transport;

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cf::cli::replay_command(replay, out, err), 0) << err.str();
  const auto document = nlohmann::json::parse(out.str());
  EXPECT_EQ(document["status"], 200);
  EXPECT_EQ(document["model"], "replay-model");
  EXPECT_EQ(document["response"]["choices"][0]["message"]["content"], "replayed");

  ASSERT_EQ(replay_transport.requests().size(), 1u);
  const auto& sent = replay_transport.requests()[0];
  EXPECT_EQ(sent.url, "https://api.fireworks.test/v1/chat/completions");
  const auto body = nlohmann::json::parse(sent.body);
  EXPECT_EQ(body["model"], "replay-model");
  EXPECT_EQ(body["messages"][0]["content"], "replay me");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
