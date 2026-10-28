#include "cf/rag/embedder.hpp"

#include <utility>

namespace cf::rag {

FireworksEmbedder::FireworksEmbedder(llm::FireworksClient& client,
                                     std::string model, std::string run_id)
    : client_(client), model_(std::move(model)), run_id_(std::move(run_id)) {}

Result<std::vector<std::vector<float>>> FireworksEmbedder::embed(
    const std::vector<std::string>& texts) {
  if (texts.empty()) {
    return std::vector<std::vector<float>>{};
  }
  llm::EmbedRequest request;
  request.model = model_;
  request.input = texts;
  request.context.run_id = run_id_;
  request.context.role = "rag";
  auto response = client_.embed(request);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return std::move(response->embeddings);
}

}  // namespace cf::rag
