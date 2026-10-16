// Unit tests for GitHub App authentication (design section 10.4.2): RS256
// JWTs, installation-token exchange, caching and refresh.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <nlohmann/json.hpp>

#include "cf/github/app_auth.hpp"
#include "cf/github/client.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::AppAuth;
using cf::github::AppAuthOptions;
using cf::github::Client;
using cf::github::ClientOptions;
using cf::test::ScriptedHttpTransport;

struct KeyPair {
  std::string private_pem;
  EVP_PKEY* public_key = nullptr;

  KeyPair() = default;
  KeyPair(const KeyPair&) = delete;
  KeyPair& operator=(const KeyPair&) = delete;
  KeyPair(KeyPair&& other) noexcept
      : private_pem(std::move(other.private_pem)), public_key(other.public_key) {
    other.public_key = nullptr;
  }
  KeyPair& operator=(KeyPair&& other) noexcept {
    if (this != &other) {
      if (public_key != nullptr) {
        EVP_PKEY_free(public_key);
      }
      private_pem = std::move(other.private_pem);
      public_key = other.public_key;
      other.public_key = nullptr;
    }
    return *this;
  }
  ~KeyPair() {
    if (public_key != nullptr) {
      EVP_PKEY_free(public_key);
    }
  }
};

KeyPair generate_key_pair() {
  KeyPair pair;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY* key = nullptr;
  if (ctx != nullptr && EVP_PKEY_keygen_init(ctx) == 1 &&
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1 &&
      EVP_PKEY_keygen(ctx, &key) == 1) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    pair.private_pem.assign(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    pair.public_key = key;
    key = nullptr;
  }
  if (key != nullptr) {
    EVP_PKEY_free(key);
  }
  if (ctx != nullptr) {
    EVP_PKEY_CTX_free(ctx);
  }
  return pair;
}

std::string base64url_decode(std::string text) {
  for (char& c : text) {
    if (c == '-') {
      c = '+';
    } else if (c == '_') {
      c = '/';
    }
  }
  while (text.size() % 4 != 0) {
    text.push_back('=');
  }
  std::string out(text.size(), '\0');
  const int written =
      EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                      reinterpret_cast<const unsigned char*>(text.data()),
                      static_cast<int>(text.size()));
  // EVP_DecodeBlock counts padding bytes as output; strip them.
  std::size_t real = static_cast<std::size_t>(written);
  while (real > 0 && out[real - 1] == '\0') {
    --real;
  }
  out.resize(real);
  return out;
}

bool verify_rs256(const std::string& message, const std::string& signature,
                  EVP_PKEY* public_key) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = ctx != nullptr &&
            EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, public_key) == 1 &&
            EVP_DigestVerify(ctx,
                             reinterpret_cast<const unsigned char*>(signature.data()),
                             signature.size(),
                             reinterpret_cast<const unsigned char*>(message.data()),
                             message.size()) == 1;
  if (ctx != nullptr) {
    EVP_MD_CTX_free(ctx);
  }
  return ok;
}

class AppAuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    keys_ = generate_key_pair();
    ASSERT_FALSE(keys_.private_pem.empty());
    transport_ = std::make_unique<ScriptedHttpTransport>();
    clock_ = std::chrono::system_clock::time_point{std::chrono::seconds(1'000'000)};
  }

  AppAuthOptions options() {
    AppAuthOptions options;
    options.app_id = "123456";
    options.installation_id = "42";
    options.private_key_pem = keys_.private_pem;
    options.api_base = "https://api.github.test";
    options.now_fn = [this] { return clock_; };
    return options;
  }

  KeyPair keys_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
  std::chrono::system_clock::time_point clock_;
};

TEST_F(AppAuthTest, BuildsAVerifiableRs256Jwt) {
  AppAuth auth(options(), *transport_);
  auto jwt = auth.build_jwt();
  ASSERT_TRUE(jwt.has_value()) << jwt.error().message;

  const std::size_t first_dot = jwt->find('.');
  const std::size_t second_dot = jwt->find('.', first_dot + 1);
  ASSERT_NE(first_dot, std::string::npos);
  ASSERT_NE(second_dot, std::string::npos);

  const auto header =
      nlohmann::json::parse(base64url_decode(jwt->substr(0, first_dot)));
  EXPECT_EQ(header["alg"], "RS256");
  EXPECT_EQ(header["typ"], "JWT");

  const auto payload = nlohmann::json::parse(
      base64url_decode(jwt->substr(first_dot + 1, second_dot - first_dot - 1)));
  EXPECT_EQ(payload["iss"], "123456");
  const auto epoch =
      std::chrono::duration_cast<std::chrono::seconds>(clock_.time_since_epoch())
          .count();
  EXPECT_EQ(payload["iat"], epoch - 60);
  EXPECT_EQ(payload["exp"], epoch + 540);

  const std::string signature = base64url_decode(jwt->substr(second_dot + 1));
  EXPECT_TRUE(verify_rs256(jwt->substr(0, second_dot), signature, keys_.public_key));
  // A tampered payload no longer verifies.
  EXPECT_FALSE(verify_rs256(jwt->substr(0, first_dot) + ".tampered", signature,
                            keys_.public_key));
}

TEST_F(AppAuthTest, ExchangesTheJwtForAnInstallationTokenAndCachesIt) {
  transport_->script(201,
                     R"({"token": "ghs_installation_1",
                         "expires_at": "1970-01-12T14:00:00Z"})");  // epoch+3600
  AppAuth auth(options(), *transport_);

  auto header = auth.authorization_header();
  ASSERT_TRUE(header.has_value()) << header.error().message;
  EXPECT_EQ(*header, "Bearer ghs_installation_1");

  ASSERT_EQ(transport_->requests().size(), 1u);
  EXPECT_EQ(transport_->requests()[0].method, "POST");
  EXPECT_EQ(transport_->requests()[0].url,
            "https://api.github.test/app/installations/42/access_tokens");
  bool bearer_jwt = false;
  for (const auto& [key, value] : transport_->requests()[0].headers) {
    if (key == "Authorization") {
      bearer_jwt = value.rfind("Bearer ", 0) == 0 &&
                   value.find('.') != std::string::npos;
    }
  }
  EXPECT_TRUE(bearer_jwt);

  // A second call is served from the cache.
  header = auth.authorization_header();
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(transport_->requests().size(), 1u);
}

TEST_F(AppAuthTest, RefreshesBeforeExpiry) {
  transport_->script(201,
                     R"({"token": "ghs_first",
                         "expires_at": "1970-01-12T14:00:00Z"})");  // epoch+3600
  AppAuth auth(options(), *transport_);
  ASSERT_TRUE(auth.installation_token().has_value());

  clock_ += std::chrono::seconds(3600);  // past expiry minus the margin
  transport_->script(201,
                     R"({"token": "ghs_second",
                         "expires_at": "1970-01-12T15:00:00Z"})");
  auto token = auth.installation_token();
  ASSERT_TRUE(token.has_value()) << token.error().message;
  EXPECT_EQ(*token, "ghs_second");
  EXPECT_EQ(transport_->requests().size(), 2u);
}

TEST_F(AppAuthTest, InvalidPrivateKeyFails) {
  AppAuthOptions bad = options();
  bad.private_key_pem = "not a key";
  AppAuth auth(bad, *transport_);
  auto token = auth.installation_token();
  ASSERT_FALSE(token.has_value());
  EXPECT_EQ(token.error().code, cf::github::error_codes::kAuthKeyInvalid);
  EXPECT_TRUE(transport_->requests().empty());
}

TEST_F(AppAuthTest, TokenExchangeFailureIsReported) {
  transport_->script(401, R"({"message": "Bad credentials"})");
  AppAuth auth(options(), *transport_);
  auto token = auth.installation_token();
  ASSERT_FALSE(token.has_value());
  EXPECT_EQ(token.error().code, cf::github::error_codes::kAuthTokenFailed);
}

TEST_F(AppAuthTest, ClientUsesTheTokenProvider) {
  ClientOptions client_options;
  client_options.api_base = "https://api.github.test";
  client_options.token_provider = []() -> cf::Result<std::string> {
    return std::string("ghs_dynamic");
  };
  Client client(client_options, *transport_);
  transport_->script(201,
                     R"({"number": 1, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");
  ASSERT_TRUE(client.create_issue("o", "r", "t", "b", {}).has_value());
  bool dynamic = false;
  for (const auto& [key, value] : transport_->requests()[0].headers) {
    if (key == "Authorization") {
      dynamic = value == "Bearer ghs_dynamic";
    }
  }
  EXPECT_TRUE(dynamic);
}

}  // namespace
