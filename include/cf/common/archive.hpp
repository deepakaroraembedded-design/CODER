// cf-common: encrypted, compressed archive files (design sections 6.11, 14).
//
// The format is shared by the LLM prompt archive and the opencode agent
// traffic archive: gzip-compressed plaintext sealed with AES-256-GCM and a
// small header (magic, version, original size, nonce, tag). Keys are 32 bytes
// supplied as 64 hexadecimal characters by an environment variable.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cf::common::archive {

using Key = std::vector<unsigned char>;

// Reads and decodes the key from `env_name`; nullopt when unset or invalid.
std::optional<Key> key_from_env(const std::string& env_name);

// Generates a random id such as `call_1a2b3c4d5e6f7788`.
std::string new_id(std::string_view prefix);

// Writes `plaintext` to `path`, creating parent directories. Returns false on
// any I/O or crypto failure; callers decide whether archiving is mandatory.
bool write_encrypted(const std::filesystem::path& path, const Key& key,
                     std::string_view plaintext);

// Reads and verifies a file written by write_encrypted. Nullopt on I/O,
// format, authentication or decompression failure.
std::optional<std::string> read_encrypted(const std::filesystem::path& path,
                                          const Key& key);

}  // namespace cf::common::archive
