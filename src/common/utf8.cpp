#include "cf/common/utf8.hpp"

#include <cstddef>

namespace cf {
namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

bool continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

// Length of the valid sequence starting at `begin`, or 0 when the bytes are
// not a well-formed UTF-8 scalar value (overlong forms, surrogates and values
// above U+10FFFF are rejected).
std::size_t valid_length(std::string_view text, std::size_t begin) {
  const auto byte = [&](std::size_t offset) {
    return static_cast<unsigned char>(text[begin + offset]);
  };
  const unsigned char first = byte(0);
  if (first < 0x80) {
    return 1;
  }
  if (first >= 0xC2 && first <= 0xDF) {
    return (begin + 1 < text.size() && continuation(byte(1))) ? 2 : 0;
  }
  if (first == 0xE0) {
    return (begin + 2 < text.size() && byte(1) >= 0xA0 && byte(1) <= 0xBF &&
            continuation(byte(2)))
               ? 3
               : 0;
  }
  if ((first >= 0xE1 && first <= 0xEC) || (first >= 0xEE && first <= 0xEF)) {
    return (begin + 2 < text.size() && continuation(byte(1)) &&
            continuation(byte(2)))
               ? 3
               : 0;
  }
  if (first == 0xED) {
    return (begin + 2 < text.size() && byte(1) >= 0x80 && byte(1) <= 0x9F &&
            continuation(byte(2)))
               ? 3
               : 0;
  }
  if (first == 0xF0) {
    return (begin + 3 < text.size() && byte(1) >= 0x90 && byte(1) <= 0xBF &&
            continuation(byte(2)) && continuation(byte(3)))
               ? 4
               : 0;
  }
  if (first >= 0xF1 && first <= 0xF3) {
    return (begin + 3 < text.size() && continuation(byte(1)) &&
            continuation(byte(2)) && continuation(byte(3)))
               ? 4
               : 0;
  }
  if (first == 0xF4) {
    return (begin + 3 < text.size() && byte(1) >= 0x80 && byte(1) <= 0x8F &&
            continuation(byte(2)) && continuation(byte(3)))
               ? 4
               : 0;
  }
  return 0;
}

}  // namespace

std::string sanitize_utf8(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size();) {
    const std::size_t length = valid_length(input, i);
    if (length == 0) {
      // Consume the malformed sequence as one replacement: if this byte could
      // start a multi-byte sequence, its continuation bytes belong to it too.
      const unsigned char first = static_cast<unsigned char>(input[i]);
      std::size_t skip = 1;
      if (first >= 0xC2 && first <= 0xF4) {
        while (i + skip < input.size() &&
               continuation(static_cast<unsigned char>(input[i + skip]))) {
          ++skip;
        }
      }
      out.append(kReplacement);
      i += skip;
    } else {
      out.append(input.substr(i, length));
      i += length;
    }
  }
  return out;
}

}  // namespace cf
