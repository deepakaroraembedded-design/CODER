// cf-common: UTF-8 validation for text crossing JSON and agent boundaries.
#pragma once

#include <string>
#include <string_view>

namespace cf {

// Replaces each invalid UTF-8 byte sequence with U+FFFD. Valid text is
// returned unchanged. Agent and tool output is untrusted and may contain
// arbitrary bytes, while JSON serialisation requires valid UTF-8.
std::string sanitize_utf8(std::string_view input);

}  // namespace cf
