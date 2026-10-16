#include "cf/common/hash.hpp"

#include <array>
#include <cstddef>

#include <openssl/evp.h>

namespace cf {
namespace {

std::string to_hex(const unsigned char* bytes, std::size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(len * 2, '0');
  for (std::size_t i = 0; i < len; ++i) {
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 0x0f];
  }
  return out;
}

}  // namespace

std::string sha256_hex(std::string_view data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  const int ok = EVP_Digest(data.data(), data.size(), digest.data(), &digest_len,
                            EVP_sha256(), nullptr);
  if (ok != 1) {
    return {};
  }
  return to_hex(digest.data(), digest_len);
}

std::string idempotency_key(std::string_view run_id, std::string_view milestone_id,
                            std::string_view kind, std::string_view input_digest) {
  std::string canonical;
  canonical.reserve(run_id.size() + milestone_id.size() + kind.size() +
                    input_digest.size() + 3);
  canonical.append(run_id).push_back('\n');
  canonical.append(milestone_id).push_back('\n');
  canonical.append(kind).push_back('\n');
  canonical.append(input_digest);
  return sha256_hex(canonical);
}

}  // namespace cf
