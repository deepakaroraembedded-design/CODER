// cf-rag: embedding interface and the Fireworks adapter (design section 6.3).
#pragma once

#include <string>
#include <vector>

#include "cf/common/result.hpp"
#include "cf/llm/client.hpp"

namespace cf::rag {

class Embedder {
 public:
  virtual ~Embedder() = default;
  virtual Result<std::vector<std::vector<float>>> embed(
      const std::vector<std::string>& texts) = 0;
};

// Embeddings through the OpenAI-compatible client (Fireworks, or local vLLM
// in on-premises runs; design section 13.1).
class FireworksEmbedder final : public Embedder {
 public:
  FireworksEmbedder(llm::FireworksClient& client, std::string model,
                    std::string run_id = {});

  Result<std::vector<std::vector<float>>> embed(
      const std::vector<std::string>& texts) override;

 private:
  llm::FireworksClient& client_;
  std::string model_;
  std::string run_id_;
};

}  // namespace cf::rag
