// Error handling across module boundaries (design section 4.1).
//
// CF uses tl::expected (vcpkg: tl-expected), whose interface matches C++23
// std::expected, so moving to the standard type later is a rename.
#pragma once

#include <string>
#include <type_traits>
#include <utility>

#include <tl/expected.hpp>

namespace cf {

struct Error {
  std::string code;     // stable identifier, e.g. "RAG_INDEX_MISSING"
  std::string message;  // human-readable detail
};

template <typename T>
using Result = tl::expected<T, Error>;

using Status = Result<void>;

inline tl::unexpected<Error> fail(std::string code, std::string msg) {
  return tl::make_unexpected(Error{std::move(code), std::move(msg)});
}

// Template contract (design section 4.1): references and error-like types are
// rejected at compile time so success and failure can never be confused.
namespace detail {

template <typename T>
constexpr bool is_result_value_type() {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  if (std::is_same_v<U, Error>) return false;
  if (std::is_convertible_v<U, Error>) return false;
  if (std::is_convertible_v<Error, U>) return false;
  return true;
}

}  // namespace detail

template <typename T>
struct ResultValueContract {
  static_assert(!std::is_reference_v<T>,
                "cf::Result<T> cannot hold a reference; return a pointer or "
                "std::reference_wrapper");
  static_assert(detail::is_result_value_type<T>(),
                "cf::Result<T> cannot hold Error or a type convertible to or "
                "from Error");
};

// Force instantiation of the contract for the common value types used across
// the framework; any new module that defines a Result<T> gets the same checks
// through this alias.
template <typename T>
using CheckedResult = Result<T>;

}  // namespace cf
