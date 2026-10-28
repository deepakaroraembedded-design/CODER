// Unit tests for the Fireworks client (design section 6.6): response parsing,
// retry policy, backoff, Retry-After, concurrency limits and usage accounting.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/llm/client.hpp"

namespace {

using cf::llm::CallRecord;
using cf::llm::ChatRequest;
using cf::llm::ClientOptions;
using cf::llm::EmbedRequest;
using cf::llm::FireworksClient;
using cf::llm::HttpRequest;
using cf::llm::HttpResponse;
using cf::llm::HttpTransport;
using cf::llm::SleepFunction;
using cf::llm::UsageCallback;
using Json = nlohmann::json;

struct ScriptedResponse {
  bool transport_error = false;
  std::string error_code = "LLM_TRANSPORT_ERROR";
  long status = 200;
  std::string body = "{}";
  std::vector<std::pair<std::string, std::string>> headers;
};

class FakeTransport final : public HttpTransport {
 public:
  cf::Result<HttpResponse> post(const HttpRequest& request) override {
    ScriptedResponse scripted;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(request);
      ++in_flight_;
      max_in_flight_ = std::max(max_in_flight_, in_flight_);
      if (responses_.empty()) {
        --in_flight_;
        return cf::fail("FAKE_NO_SCRIPT", "no scripted response left");
      }
      scripted = responses_.front();
      responses_.pop_front();
    }
    if (block_.count() > 0) {
      std::this_thread::sleep_for(block_);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    if (scripted.transport_error) {
      return cf::fail(scripted.error_code, "scripted transport failure");
    }
    HttpResponse response;
    response.status = scripted.status;
    response.headers = std::move(scripted.headers);
    response.body = std::move(scripted.body);
    return response;
  }

  void script(ScriptedResponse response) {
    std::lock_guard<std::mutex> lock(mutex_);
    responses_.push_back(std::move(response));
  }

  std::vector<HttpRequest> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

  int max_in_flight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_in_flight_;
  }

  void set_block(std::chrono::milliseconds block) { block_ = block; }

 private:
  mutable std::mutex mutex_;
  std::deque<ScriptedResponse> responses_;
  std::vector<HttpRequest> requests_;
  int in_flight_ = 0;
  int max_in_flight_ = 0;
  std::chrono::milliseconds block_{0};
};

Json chat_success_body() {
  Json message = {{"role", "assistant"}, {"content", "hello"}};
  message["tool_calls"] = Json::array({
      Json{{"id", "call_1"},
           {"type", "function"},
           {"function",
            Json{{"name", "submit_work"}, {"arguments", R"({"summary":"done"})"}}}},
  });
  return Json{{"id", "chatcmpl-1"},
              {"model", "test-model"},
              {"choices", Json::array({Json{{"index", 0}, {"message", message}}})},
              {"usage",
               Json{{"prompt_tokens", 100},
                    {"completion_tokens", 20},
                    {"prompt_tokens_details", Json{{"cached_tokens", 50}}}}}};
}

Json embed_success_body() {
  return Json{
      {"data", Json::array({Json{{"embedding", Json::array({0.5, 1.5})}, {"index", 0}},
                            Json{{"embedding", Json::array({-0.25})}, {"index", 1}}})},
      {"usage", Json{{"prompt_tokens", 3}, {"total_tokens", 3}}}};
}

class LlmClientTest : public ::testing::Test {
 protected:
  ClientOptions base_options() const {
    ClientOptions options;
    options.api_key = "test-key";
    options.base_url = "https://api.fireworks.ai/inference/v1";
    options.max_attempts = 3;
    options.initial_backoff = std::chrono::milliseconds(1);
    options.max_backoff = std::chrono::milliseconds(4);
    options.prices["test-model"] = cf::llm::ModelPrice{1.0, 2.0};
    return options;
  }

  std::unique_ptr<FireworksClient> make_client(
      FakeTransport*& transport, const ClientOptions& options,
      std::vector<CallRecord>* records = nullptr,
      std::vector<std::chrono::milliseconds>* delays = nullptr) {
    auto fake = std::make_unique<FakeTransport>();
    transport = fake.get();
    UsageCallback callback;
    if (records != nullptr) {
      callback = [records](const CallRecord& record) { records->push_back(record); };
    }
    SleepFunction sleeper;
    if (delays != nullptr) {
      sleeper = [delays](std::chrono::milliseconds delay) { delays->push_back(delay); };
    }
    return std::make_unique<FireworksClient>(options, std::move(fake),
                                             std::move(callback), std::move(sleeper));
  }

  static ChatRequest chat_request() {
    ChatRequest request;
    request.model = "test-model";
    request.messages.push_back(cf::llm::ChatMessage{
        "user", "do the task", Json(), "", ""});
    request.context = {"R12", "coder", "", "artifact://runs/R12/prompts/1"};
    return request;
  }
};

TEST_F(LlmClientTest, ArchivesEncryptedPromptsAndResponses) {
  class KeyGuard {
   public:
    KeyGuard() {
      setenv("CF_ARCHIVE_KEY",
             "00112233445566778899aabbccddeeff"
             "00112233445566778899aabbccddeeff",
             1);
    }
    ~KeyGuard() { unsetenv("CF_ARCHIVE_KEY"); }
  } key_guard;

  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_llm_archive_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  ClientOptions options = base_options();
  options.archive_dir = dir;
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  auto client = make_client(transport, options, &records);
  transport->script(ScriptedResponse{false, "", 200, chat_success_body().dump(), {}});
  auto response = client->chat(chat_request());
  ASSERT_TRUE(response.has_value()) << response.error().message;
  ASSERT_EQ(records.size(), 1u);
  ASSERT_FALSE(records[0].prompt_ref.empty());

  auto archived = cf::llm::read_archived_call(records[0].prompt_ref);
  ASSERT_TRUE(archived.has_value()) << archived.error().message;
  EXPECT_EQ(archived->run_id, "R12");
  EXPECT_EQ(archived->role, "coder");
  EXPECT_EQ(archived->model, "test-model");
  EXPECT_EQ(archived->status, 200);
  EXPECT_EQ(archived->request["model"], "test-model");
  EXPECT_TRUE(archived->response.contains("choices"));
  EXPECT_EQ(archived->usage.prompt_tokens, 100);
  EXPECT_EQ(archived->usage.completion_tokens, 20);

  std::filesystem::remove_all(dir, ec);
}

TEST_F(LlmClientTest, ChatParsesContentToolCallsAndUsage) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  auto client = make_client(transport, base_options(), &records);
  transport->script(ScriptedResponse{false, "", 200, chat_success_body().dump(), {}});

  auto response = client->chat(chat_request());
  ASSERT_TRUE(response.has_value()) << response.error().message;
  EXPECT_EQ(response->content, "hello");
  ASSERT_EQ(response->tool_calls.size(), 1u);
  EXPECT_EQ(response->tool_calls[0].id, "call_1");
  EXPECT_EQ(response->tool_calls[0].name, "submit_work");
  EXPECT_EQ(response->tool_calls[0].arguments["summary"], "done");
  EXPECT_EQ(response->usage.prompt_tokens, 100);
  EXPECT_EQ(response->usage.completion_tokens, 20);
  EXPECT_EQ(response->usage.cached_tokens, 50);

  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].run_id, "R12");
  EXPECT_EQ(records[0].role, "coder");
  EXPECT_EQ(records[0].prompt_ref, "artifact://runs/R12/prompts/1");
  EXPECT_EQ(records[0].attempts, 1);
  EXPECT_EQ(records[0].status, 200);
  EXPECT_NEAR(records[0].usd, 0.00014, 1e-12);

  const auto requests = transport->requests();
  ASSERT_EQ(requests.size(), 1u);
  ASSERT_FALSE(requests[0].headers.empty());
  bool has_auth = false;
  for (const auto& [key, value] : requests[0].headers) {
    if (key == "Authorization") {
      has_auth = value == "Bearer test-key";
    }
  }
  EXPECT_TRUE(has_auth);
}

TEST_F(LlmClientTest, RetryOn429HonoursRetryAfter) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  std::vector<std::chrono::milliseconds> delays;
  auto client = make_client(transport, base_options(), &records, &delays);
  transport->script(ScriptedResponse{
      false, "", 429, R"({"error":"rate limited"})", {{"Retry-After", "2"}}});
  transport->script(ScriptedResponse{false, "", 200, chat_success_body().dump(), {}});

  auto response = client->chat(chat_request());
  ASSERT_TRUE(response.has_value()) << response.error().message;
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].attempts, 2);
  ASSERT_EQ(delays.size(), 1u);
  EXPECT_EQ(delays[0], std::chrono::milliseconds(2000));
}

TEST_F(LlmClientTest, RetryOnServerErrorThenSucceeds) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  std::vector<std::chrono::milliseconds> delays;
  auto client = make_client(transport, base_options(), &records, &delays);
  transport->script(ScriptedResponse{false, "", 500, "internal error", {}});
  transport->script(ScriptedResponse{false, "", 200, chat_success_body().dump(), {}});

  auto response = client->chat(chat_request());
  ASSERT_TRUE(response.has_value()) << response.error().message;
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].attempts, 2);
  EXPECT_EQ(delays.size(), 1u);
  EXPECT_LE(delays[0], std::chrono::milliseconds(4));
}

TEST_F(LlmClientTest, ClientErrorIsNotRetried) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  auto client = make_client(transport, base_options(), &records);
  transport->script(ScriptedResponse{false, "", 400, R"({"error":"bad request"})", {}});

  auto response = client->chat(chat_request());
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(response.error().code, cf::llm::error_codes::kHttpError);
  EXPECT_EQ(transport->requests().size(), 1u);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].attempts, 1);
  EXPECT_EQ(records[0].status, 400);
}

TEST_F(LlmClientTest, RetriesExhaustedReportFailure) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  auto client = make_client(transport, base_options(), &records);
  for (int i = 0; i < 3; ++i) {
    transport->script(ScriptedResponse{false, "", 429, "rate limited", {}});
  }

  auto response = client->chat(chat_request());
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(response.error().code, cf::llm::error_codes::kRetryExhausted);
  EXPECT_EQ(transport->requests().size(), 3u);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].attempts, 3);
}

TEST_F(LlmClientTest, TransportFailureIsRetried) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  auto client = make_client(transport, base_options(), &records);
  transport->script(ScriptedResponse{true, "LLM_TRANSPORT_ERROR", 0, "", {}});
  transport->script(ScriptedResponse{false, "", 200, chat_success_body().dump(), {}});

  auto response = client->chat(chat_request());
  ASSERT_TRUE(response.has_value()) << response.error().message;
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].attempts, 2);
}

TEST_F(LlmClientTest, MissingApiKeyFailsWithoutSending) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  ClientOptions options = base_options();
  options.api_key.clear();
  auto client = make_client(transport, options, &records);

  auto response = client->chat(chat_request());
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(response.error().code, cf::llm::error_codes::kNoApiKey);
  EXPECT_TRUE(transport->requests().empty());
  EXPECT_TRUE(records.empty());
}

TEST_F(LlmClientTest, InvalidJsonResponseFails) {
  FakeTransport* transport = nullptr;
  auto client = make_client(transport, base_options());
  transport->script(ScriptedResponse{false, "", 200, "not json", {}});

  auto response = client->chat(chat_request());
  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(response.error().code, cf::llm::error_codes::kInvalidResponse);
}

TEST_F(LlmClientTest, EmbedParsesVectors) {
  FakeTransport* transport = nullptr;
  std::vector<CallRecord> records;
  auto client = make_client(transport, base_options(), &records);
  transport->script(ScriptedResponse{false, "", 200, embed_success_body().dump(), {}});

  EmbedRequest request;
  request.model = "test-model";
  request.input = {"alpha", "beta"};
  request.context = {"R12", "planner", "", ""};
  auto response = client->embed(request);
  ASSERT_TRUE(response.has_value()) << response.error().message;
  ASSERT_EQ(response->embeddings.size(), 2u);
  ASSERT_EQ(response->embeddings[0].size(), 2u);
  EXPECT_FLOAT_EQ(response->embeddings[0][0], 0.5f);
  EXPECT_FLOAT_EQ(response->embeddings[0][1], 1.5f);
  EXPECT_FLOAT_EQ(response->embeddings[1][0], -0.25f);
  EXPECT_EQ(response->usage.prompt_tokens, 3);

  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].role, "planner");
  const auto requests = transport->requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_NE(requests[0].url.find("/embeddings"), std::string::npos);
}

TEST_F(LlmClientTest, ConcurrencyLimitIsEnforced) {
  FakeTransport* transport = nullptr;
  ClientOptions options = base_options();
  options.max_concurrency = 2;
  options.max_attempts = 1;
  auto client = make_client(transport, options);
  transport->set_block(std::chrono::milliseconds(20));
  for (int i = 0; i < 6; ++i) {
    transport->script(ScriptedResponse{false, "", 200, chat_success_body().dump(), {}});
  }

  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&client, &failures] {
      auto response = client->chat(chat_request());
      if (!response.has_value()) {
        ++failures;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_LE(transport->max_in_flight(), 2);
}

}  // namespace
