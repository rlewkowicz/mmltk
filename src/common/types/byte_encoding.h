#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
namespace mmltk::common::types {
[[nodiscard]] inline std::string hex_encode(const std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string encoded(bytes.size() * 2U, '\0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        encoded[index * 2U] = kDigits[bytes[index] >> 4U];
        encoded[index * 2U + 1U] = kDigits[bytes[index] & 0x0FU];
    }
    return encoded;
}
}  // namespace mmltk::common::types
