#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
namespace mmltk::common::types {
inline void accept_fixed_buffer_conversion(const char* const begin, std::size_t& size, const std::to_chars_result converted) noexcept {
    if (converted.ec == std::errc{}) size = static_cast<std::size_t>(converted.ptr - begin);
}
inline void append_integer_to_fixed_buffer(const std::span<char> bytes, std::size_t& size, const std::int64_t value) noexcept {
    accept_fixed_buffer_conversion(
        bytes.data(), size, std::to_chars(bytes.data() + static_cast<std::ptrdiff_t>(size), bytes.data() + static_cast<std::ptrdiff_t>(bytes.size()), value));
}
inline void append_integer_to_fixed_buffer(const std::span<char> bytes, std::size_t& size, const std::uint64_t value) noexcept {
    accept_fixed_buffer_conversion(
        bytes.data(), size, std::to_chars(bytes.data() + static_cast<std::ptrdiff_t>(size), bytes.data() + static_cast<std::ptrdiff_t>(bytes.size()), value));
}
}  // namespace mmltk::common::types
