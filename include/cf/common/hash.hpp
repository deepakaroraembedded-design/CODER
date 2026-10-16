// cf-common: content hashing for idempotency keys and digests
// (design section 6.12.1).
#pragma once

#include <string>
#include <string_view>

namespace cf {

// Lowercase hex SHA-256 of the input bytes.
std::string sha256_hex(std::string_view data);

// Canonical idempotency key for an external operation (design section 6.12.1):
// sha256(run_id, milestone_id, kind, input_digest). Derived from what the
// operation does, not from when it runs, so a retry after a crash can be
// recognised as a retry.
std::string idempotency_key(std::string_view run_id, std::string_view milestone_id,
                            std::string_view kind, std::string_view input_digest);

}  // namespace cf
