// cf-github: GitHub App authentication (design section 10.4.2).
//
// The bot authenticates as a GitHub App: a short-lived RS256 JWT (the app
// identity) is exchanged for an installation token (valid for up to one
// hour), which is cached and refreshed before it expires. The private key
// stays in memory: it is never written to disk, logs or prompts.
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "cf/common/result.hpp"
#include "cf/llm/client.hpp"

namespace cf::github {

namespace error_codes {
inline constexpr const char* kAuthKeyInvalid = "GITHUB_AUTH_KEY_INVALID";
inline constexpr const char* kAuthTokenFailed = "GITHUB_AUTH_TOKEN_FAILED";
}  // namespace error_codes

struct AppAuthOptions {
  std::string app_id;
  std::string installation_id;
  std::string private_key_pem;  // PKCS#8 or PKCS#1 PEM, in memory only
  std::string api_base = "https://api.github.com";
  std::chrono::milliseconds timeout{30000};
  // Renew this long before the token actually expires.
  std::chrono::seconds refresh_margin{60};
  // Injectable clock for tests.
  std::function<std::chrono::system_clock::time_point()> now_fn;
};

class AppAuth {
 public:
  AppAuth(AppAuthOptions options, llm::HttpTransport& transport);

  // The current installation token, fetched or refreshed as needed.
  Result<std::string> installation_token();

  // "Bearer <installation token>" for API calls.
  Result<std::string> authorization_header();

  // The app JWT (exposed for tests): header.payload.signature, base64url.
  Result<std::string> build_jwt() const;

 private:
  Result<std::string> exchange();
  std::chrono::system_clock::time_point now() const;

  AppAuthOptions options_;
  llm::HttpTransport& transport_;
  std::string cached_token_;
  std::chrono::system_clock::time_point cached_until_{};
};

}  // namespace cf::github
