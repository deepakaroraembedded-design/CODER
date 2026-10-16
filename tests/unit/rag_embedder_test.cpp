// Unit tests for the Fireworks embedding adapter (design section 6.3).
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cf/rag/embedder.hpp"

namespace {

class FakeTransport final : public cf::llm::HttpTransport {
 public:
  cf::Result<cf::llm::HttpResponse> post(const cf::llm::HttpRequest& request) override {
    last_body = nlohmann::json::parse(request.body);
    cf::llm::HttpResponse response;
    response.status = 200;
    response.body =
        R"({"data":[{"embedding":[0.5,1.0],"index":0},{"embedding":[1.5],"index":1}],)"
        R"("usage":{"prompt_tokens":4,"total_tokens":4}})";
    return response;
  }

  nlohmann::json last_body;
};

TEST(RagEmbedderTest, MapsEmbeddingsAndForwardsTheModel) {
  auto transport = std::make_unique<FakeTransport>();
  FakeTransport* raw = transport.get();
  cf::llm::ClientOptions options;
  options.api_key = "test-key";
  options.base_url = "https://example.invalid/v1";
  options.max_attempts = 1;
  cf::llm::FireworksClient client(options, std::move(transport));

  cf::rag::FireworksEmbedder embedder(client, "embed-model");
  const auto vectors = embedder.embed({"alpha", "beta"});
  ASSERT_TRUE(vectors.has_value()) << vectors.error().message;
  ASSERT_EQ(vectors->size(), 2u);
  ASSERT_EQ((*vectors)[0].size(), 2u);
  EXPECT_FLOAT_EQ((*vectors)[0][0], 0.5f);
  EXPECT_FLOAT_EQ((*vectors)[1][0], 1.5f);
  EXPECT_EQ(raw->last_body["model"], "embed-model");
  ASSERT_EQ(raw->last_body["input"].size(), 2u);
}

TEST(RagEmbedderTest, EmptyInputSkipsTheCall) {
  auto transport = std::make_unique<FakeTransport>();
  cf::llm::ClientOptions options;
  options.api_key = "test-key";
  options.base_url = "https://example.invalid/v1";
  cf::llm::FireworksClient client(options, std::move(transport));
  cf::rag::FireworksEmbedder embedder(client, "embed-model");
  const auto vectors = embedder.embed({});
  ASSERT_TRUE(vectors.has_value());
  EXPECT_TRUE(vectors->empty());
}

}  // namespace
