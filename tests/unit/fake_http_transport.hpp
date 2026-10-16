// Shared scripted HTTP transport for tests (GitHub, providers).
#pragma once

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "cf/llm/client.hpp"

namespace cf::test {

class ScriptedHttpTransport final : public cf::llm::HttpTransport {
 public:
  struct RecordedRequest {
    std::string method;
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
  };

  cf::Result<cf::llm::HttpResponse> post(const cf::llm::HttpRequest& request) override {
    return respond("POST", request);
  }

  cf::Result<cf::llm::HttpResponse> get(const cf::llm::HttpRequest& request) override {
    return respond("GET", request);
  }

  cf::Result<cf::llm::HttpResponse> patch(const cf::llm::HttpRequest& request) override {
    return respond("PATCH", request);
  }

  void script(long status, std::string body) {
    cf::llm::HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    responses_.push_back(std::move(response));
  }

  void script_transport_error(std::string message) {
    transport_error_ = std::move(message);
  }

  const std::vector<RecordedRequest>& requests() const { return requests_; }

 private:
  cf::Result<cf::llm::HttpResponse> respond(const std::string& method,
                                            const cf::llm::HttpRequest& request) {
    requests_.push_back(
        RecordedRequest{method, request.url, request.body, request.headers});
    if (!transport_error_.empty()) {
      return cf::fail(cf::llm::error_codes::kTransportError, transport_error_);
    }
    if (responses_.empty()) {
      cf::llm::HttpResponse response;
      response.status = 500;
      response.body = R"({"message":"no scripted response"})";
      return response;
    }
    cf::llm::HttpResponse response = responses_.front();
    responses_.pop_front();
    return response;
  }

  std::deque<cf::llm::HttpResponse> responses_;
  std::vector<RecordedRequest> requests_;
  std::string transport_error_;
};

}  // namespace cf::test
