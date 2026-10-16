#pragma once
#include <exception>
#include <string_view>
namespace mmltk::common::types {
// The caller retains the exception pointer and fallback storage while using the view.
[[nodiscard]] inline std::string_view exception_message(const std::exception_ptr& failure, std::string_view absent, std::string_view nonstandard) noexcept {
 try {
  if (failure) std::rethrow_exception(failure);
 } catch (const std::exception& error) { return error.what(); } catch (...) {
  return nonstandard;
 }
 return absent;
}
}  // namespace mmltk::common::types
