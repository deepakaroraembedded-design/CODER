// cf-common: duration literals used by the configuration and plan schemas.
//
// Human-waiting deadlines are expressed in business days (design section
// 12.1), e.g. "5d". Weekends are excluded when the deadline is computed; the
// parser only validates the literal.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cf {

// Accepts a positive whole number followed by 'd' (surrounding blanks are
// ignored), e.g. "5d". Returns nullopt for any other form, including "0d".
inline std::optional<std::uint32_t> parse_business_day_duration(
    std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  if (text.size() < 2) {
    return std::nullopt;
  }
  const char suffix = text.back();
  if (suffix != 'd' && suffix != 'D') {
    return std::nullopt;
  }
  text.remove_suffix(1);
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::uint64_t>(digit - '0');
    if (value > 3650) {
      return std::nullopt;
    }
  }
  if (value == 0) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace cf
