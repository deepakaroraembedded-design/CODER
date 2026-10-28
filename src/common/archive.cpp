// cf-common: encrypted, compressed archive files (design sections 6.11, 14).
#include "cf/common/archive.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

namespace cf::common::archive {
namespace {

constexpr std::string_view kAad = "cf-archive-v1";
constexpr std::size_t kHeaderBytes = 40;  // magic+version+size+nonce+tag

std::vector<unsigned char> decode_key(std::string_view text) {
  if (text.size() != 64) {
    return {};
  }
  std::vector<unsigned char> key(32);
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < 32; ++i) {
    const int high = hex(text[i * 2]);
    const int low = hex(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return {};
    }
    key[i] = static_cast<unsigned char>((high << 4) | low);
  }
  return key;
}

std::optional<std::vector<unsigned char>> gzip_compress(std::string_view text) {
  uLongf size = compressBound(static_cast<uLong>(text.size()));
  std::vector<unsigned char> output(size);
  if (compress2(output.data(), &size,
                reinterpret_cast<const Bytef*>(text.data()),
                static_cast<uLong>(text.size()), Z_BEST_SPEED) != Z_OK) {
    return std::nullopt;
  }
  output.resize(size);
  return output;
}

std::optional<std::string> gzip_decompress(const unsigned char* data,
                                           std::size_t size,
                                           std::size_t original_size) {
  std::string output(original_size, '\0');
  uLongf out_size = static_cast<uLongf>(original_size);
  if (uncompress(reinterpret_cast<Bytef*>(output.data()), &out_size, data,
                 static_cast<uLong>(size)) != Z_OK ||
      out_size != original_size) {
    return std::nullopt;
  }
  return output;
}

bool aes_crypt(const std::vector<unsigned char>& key,
               const unsigned char* nonce, const unsigned char* tag,
               bool encrypt, std::string_view input, std::string& output) {
  output.resize(input.size());
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (context == nullptr) {
    return false;
  }
  const bool init =
      (encrypt ? EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr,
                                    nullptr, nullptr)
               : EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr,
                                    nullptr, nullptr)) == 1;
  bool ok = init &&
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, 12,
                                nullptr) == 1 &&
            (encrypt ? EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(),
                                          nonce)
                     : EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(),
                                          nonce)) == 1;
  int length = 0;
  if (ok) {
    auto* aad = reinterpret_cast<const unsigned char*>(kAad.data());
    ok = (encrypt ? EVP_EncryptUpdate(context, nullptr, &length, aad,
                                      static_cast<int>(kAad.size()))
                  : EVP_DecryptUpdate(context, nullptr, &length, aad,
                                       static_cast<int>(kAad.size()))) == 1;
  }
  if (ok) {
    auto* data = reinterpret_cast<unsigned char*>(output.data());
    auto* input_data = reinterpret_cast<const unsigned char*>(input.data());
    ok = (encrypt ? EVP_EncryptUpdate(context, data, &length, input_data,
                                      static_cast<int>(input.size()))
                  : EVP_DecryptUpdate(context, data, &length, input_data,
                                       static_cast<int>(input.size()))) == 1;
  }
  int final_length = 0;
  if (ok) {
    if (encrypt) {
      ok = EVP_EncryptFinal_ex(
               context,
               reinterpret_cast<unsigned char*>(output.data()) + length,
               &final_length) == 1;
      if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(
                 context, EVP_CTRL_GCM_GET_TAG, 16,
                 const_cast<unsigned char*>(tag ? tag : nullptr)) == 1;
      }
    } else {
      ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, 16,
                               const_cast<unsigned char*>(tag)) == 1;
      if (ok) {
        ok = EVP_DecryptFinal_ex(
                 context,
                 reinterpret_cast<unsigned char*>(output.data()) + length,
                 &final_length) == 1;
      }
    }
  }
  EVP_CIPHER_CTX_free(context);
  return ok;
}

void put_u32(unsigned char* out, std::uint32_t value) {
  out[0] = static_cast<unsigned char>((value >> 24) & 0xff);
  out[1] = static_cast<unsigned char>((value >> 16) & 0xff);
  out[2] = static_cast<unsigned char>((value >> 8) & 0xff);
  out[3] = static_cast<unsigned char>(value & 0xff);
}

std::uint32_t read_u32(const unsigned char* in) {
  return (static_cast<std::uint32_t>(in[0]) << 24) |
         (static_cast<std::uint32_t>(in[1]) << 16) |
         (static_cast<std::uint32_t>(in[2]) << 8) |
         static_cast<std::uint32_t>(in[3]);
}

}  // namespace

std::optional<Key> key_from_env(const std::string& env_name) {
  const char* value = std::getenv(env_name.c_str());
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  auto key = decode_key(value);
  if (key.empty()) {
    return std::nullopt;
  }
  return key;
}

std::string new_id(std::string_view prefix) {
  unsigned char bytes[8] = {0};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    std::random_device random;
    for (auto& byte : bytes) {
      byte = static_cast<unsigned char>(random());
    }
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(prefix);
  for (const unsigned char byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

bool write_encrypted(const std::filesystem::path& path, const Key& key,
                     std::string_view plaintext) {
  if (key.size() != 32) {
    return false;
  }
  auto compressed = gzip_compress(plaintext);
  if (!compressed) {
    return false;
  }
  std::string ciphertext;
  unsigned char nonce[12] = {0};
  unsigned char tag[16] = {0};
  if (RAND_bytes(nonce, sizeof(nonce)) != 1 ||
      !aes_crypt(key, nonce, tag, true,
                 std::string_view(reinterpret_cast<const char*>(compressed->data()),
                                  compressed->size()),
                 ciphertext)) {
    return false;
  }
  std::vector<unsigned char> file(kHeaderBytes + ciphertext.size(), 0);
  file[0] = 'C';
  file[1] = 'F';
  file[2] = 'A';
  file[3] = 'R';
  file[4] = 1;
  put_u32(file.data() + 8, static_cast<std::uint32_t>(plaintext.size()));
  std::memcpy(file.data() + 12, nonce, sizeof(nonce));
  std::memcpy(file.data() + 24, tag, sizeof(tag));
  std::memcpy(file.data() + kHeaderBytes, ciphertext.data(), ciphertext.size());

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(file.data()),
            static_cast<std::streamsize>(file.size()));
  return static_cast<bool>(out);
}

std::optional<std::string> read_encrypted(const std::filesystem::path& path,
                                          const Key& key) {
  if (key.size() != 32) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  if (file.size() < kHeaderBytes || file[0] != 'C' || file[1] != 'F' ||
      file[2] != 'A' || file[3] != 'R' || file[4] != 1) {
    return std::nullopt;
  }
  const std::uint32_t original_size = read_u32(file.data() + 8);
  const std::string ciphertext(
      reinterpret_cast<const char*>(file.data() + kHeaderBytes),
      file.size() - kHeaderBytes);
  std::string compressed;
  if (!aes_crypt(key, file.data() + 12, file.data() + 24, false, ciphertext,
                 compressed)) {
    return std::nullopt;
  }
  return gzip_decompress(
      reinterpret_cast<const unsigned char*>(compressed.data()),
      compressed.size(), original_size);
}

}  // namespace cf::common::archive
