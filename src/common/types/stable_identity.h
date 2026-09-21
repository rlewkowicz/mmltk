#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace mmltk::common::types {
inline constexpr std::uint64_t kOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kPrime = 1099511628211ULL;
[[nodiscard]] constexpr std::uint64_t append_byte(std::uint64_t value, const unsigned char byte) noexcept {
 value ^= byte;
 return value * kPrime;
}
[[nodiscard]] constexpr std::uint64_t append(std::uint64_t value, const std::string_view bytes) noexcept {
 for (const unsigned char byte : bytes) { value = append_byte(value, byte); }
 return value;
}
template <std::unsigned_integral Integer>
[[nodiscard]] constexpr std::uint64_t append_little_endian(std::uint64_t value, const Integer integer) noexcept {
 for (std::size_t index = 0U; index < sizeof(Integer); ++index) { value = append_byte(value, static_cast<unsigned char>(integer >> (index * 8U))); }
 return value;
}
[[nodiscard]] constexpr std::uint64_t from_text(const std::string_view text) noexcept { return append(kOffset, text); }
[[nodiscard]] constexpr std::uint64_t nonzero(const std::uint64_t value) noexcept { return value == 0U ? 1U : value; }
}  // namespace mmltk::common::types
