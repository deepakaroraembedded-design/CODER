#include "cf/github/app_auth.hpp"

#include <cstdio>
#include <ctime>
#include <utility>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <nlohmann/json.hpp>

namespace cf::github {
namespace {

using Json = nlohmann::json;

std::string base64url(const unsigned char* data, std::size_t size) {
  if (size == 0) {
    return {};
  }
  const std::size_t encoded_size = 4 * ((size + 2) / 3);
  std::string out(encoded_size, '\0');
  const int written = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
  out.resize(static_cast<std::size_t>(written));
  for (char& c : out) {
    if (c == '+') {
      c = '-';
    } else if (c == '/') {
      c = '_';
    }
  }
  while (!out.empty() && out.back() == '=') {
    out.pop_back();
  }
  return out;
}

std::string base64url(std::string_view text) {
  return base64url(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

// RS256-signs `message` with the PEM private key.
Result<std::string> sign_rs256(const std::string& message, const std::string& pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    return fail(error_codes::kAuthKeyInvalid, "cannot allocate a BIO for the key");
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (key == nullptr) {
    return fail(error_codes::kAuthKeyInvalid,
                "cannot parse the GitHub App private key (expects PEM)");
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  std::string signature;
  bool ok = ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr,
                                                 key) == 1;
  std::size_t signature_size = 0;
  if (ok) {
    ok = EVP_DigestSign(ctx, nullptr, &signature_size,
                        reinterpret_cast<const unsigned char*>(message.data()),
                        message.size()) == 1;
  }
  if (ok) {
    signature.resize(signature_size);
    ok = EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(signature.data()),
                        &signature_size,
                        reinterpret_cast<const unsigned char*>(message.data()),
                        message.size()) == 1;
    signature.resize(signature_size);
  }
  if (ctx != nullptr) {
    EVP_MD_CTX_free(ctx);
  }
  EVP_PKEY_free(key);
  if (!ok) {
    return fail(error_codes::kAuthKeyInvalid, "RS256 signing failed");
  }
  return signature;
}

// "2026-10-15T12:00:00Z" -> epoch seconds; 0 when unparseable.
std::int64_t parse_iso8601(const std::string& text) {
  std::tm tm{};
  const int matched = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%dZ", &tm.tm_year,
                                  &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                                  &tm.tm_sec);
  if (matched != 6) {
    return 0;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return static_cast<std::int64_t>(::timegm(&tm));
}

}  // namespace

AppAuth::AppAuth(AppAuthOptions options, llm::HttpTransport& transport)
    : options_(std::move(options)), transport_(transport) {}

std::chrono::system_clock::time_point AppAuth::now() const {
  if (options_.now_fn) {
    return options_.now_fn();
  }
  return std::chrono::system_clock::now();
}

Result<std::string> AppAuth::build_jwt() const {
  const auto now_point = now();
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                         now_point.time_since_epoch())
                         .count();
  const Json header = {{"alg", "RS256"}, {"typ", "JWT"}};
  const Json payload = {{"iat", epoch - 60},   // allow for clock skew
                        {"exp", epoch + 540},  // GitHub allows at most 10 minutes
                        {"iss", options_.app_id}};
  const std::string signing_input =
      base64url(header.dump()) + "." + base64url(payload.dump());
  auto signature = sign_rs256(signing_input, options_.private_key_pem);
  if (!signature) {
    return tl::make_unexpected(signature.error());
  }
  return signing_input + "." +
         base64url(reinterpret_cast<const unsigned char*>(signature->data()),
                   signature->size());
}

Result<std::string> AppAuth::exchange() {
  auto jwt = build_jwt();
  if (!jwt) {
    return tl::make_unexpected(jwt.error());
  }
  llm::HttpRequest request;
  request.url = options_.api_base + "/app/installations/" + options_.installation_id +
                "/access_tokens";
  request.headers = {{"Authorization", "Bearer " + *jwt},
                     {"Accept", "application/vnd.github+json"},
                     {"X-GitHub-Api-Version", "2022-11-28"},
                     {"Content-Type", "application/json"}};
  request.body = "{}";
  request.timeout = options_.timeout;

  auto response = transport_.post(request);
  if (!response) {
    return fail(error_codes::kAuthTokenFailed, response.error().message);
  }
  if (response->status < 200 || response->status >= 300) {
    return fail(error_codes::kAuthTokenFailed,
                "installation token exchange returned HTTP " +
                    std::to_string(response->status));
  }
  const Json parsed = Json::parse(response->body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return fail(error_codes::kAuthTokenFailed, "installation token response is not JSON");
  }
  const std::string token = parsed.value("token", "");
  if (token.empty()) {
    return fail(error_codes::kAuthTokenFailed,
                "installation token response has no token");
  }

  const std::int64_t expires = parse_iso8601(parsed.value("expires_at", ""));
  const auto now_point = now();
  const std::chrono::seconds lifetime =
      expires > 0 ? std::chrono::seconds(expires -
                                         std::chrono::duration_cast<std::chrono::seconds>(
                                             now_point.time_since_epoch())
                                             .count())
                  : std::chrono::seconds(1800);
  cached_token_ = token;
  cached_until_ = now_point + lifetime;
  return token;
}

Result<std::string> AppAuth::installation_token() {
  const auto now_point = now();
  if (!cached_token_.empty() &&
      now_point + options_.refresh_margin < cached_until_) {
    return cached_token_;
  }
  return exchange();
}

Result<std::string> AppAuth::authorization_header() {
  auto token = installation_token();
  if (!token) {
    return tl::make_unexpected(token.error());
  }
  return "Bearer " + *token;
}

}  // namespace cf::github
