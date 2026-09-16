#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace mmltk::testsupport {
inline constexpr std::array<unsigned char, 4U> kWebSocketTestMask{0x13U, 0x57U, 0x9bU, 0xdfU};
[[nodiscard]] inline std::size_t masked_websocket_frame_size(const std::size_t payload_bytes) noexcept {
    if (payload_bytes <= 125U) return 2U + kWebSocketTestMask.size() + payload_bytes;
    if (payload_bytes <= std::numeric_limits<std::uint16_t>::max()) return 4U + kWebSocketTestMask.size() + payload_bytes;
    return 10U + kWebSocketTestMask.size() + payload_bytes;
}
inline void append_masked_websocket_frame(std::string& frame, const unsigned char opcode, const std::span<const std::byte> payload) {
    frame.push_back(static_cast<char>(0x80U | opcode));
    if (payload.size() <= 125U) {
        frame.push_back(static_cast<char>(0x80U | payload.size()));
    } else if (payload.size() <= std::numeric_limits<std::uint16_t>::max()) {
        frame.push_back(static_cast<char>(0x80U | 126U));
        frame.push_back(static_cast<char>(payload.size() >> 8U));
        frame.push_back(static_cast<char>(payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80U | 127U));
        for (std::size_t shift = 56U;; shift -= 8U) {
            frame.push_back(static_cast<char>(payload.size() >> shift));
            if (shift == 0U) break;
        }
    }
    for (const unsigned char byte : kWebSocketTestMask) frame.push_back(static_cast<char>(byte));
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        frame.push_back(static_cast<char>(std::to_integer<unsigned char>(payload[index]) ^ kWebSocketTestMask[index % kWebSocketTestMask.size()]));
    }
}
[[nodiscard]] inline std::string masked_websocket_frame(const unsigned char opcode, const std::span<const std::byte> payload) {
    std::string frame;
    frame.reserve(masked_websocket_frame_size(payload.size()));
    append_masked_websocket_frame(frame, opcode, payload);
    return frame;
}
[[nodiscard]] inline std::vector<std::byte> length_prefixed_frame(const std::span<const std::byte> payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("test transport payload exceeds its 32-bit frame prefix");
    std::vector<std::byte> frame(4U + payload.size());
    const auto size = static_cast<std::uint32_t>(payload.size());
    frame[0] = static_cast<std::byte>(size >> 24U);
    frame[1] = static_cast<std::byte>(size >> 16U);
    frame[2] = static_cast<std::byte>(size >> 8U);
    frame[3] = static_cast<std::byte>(size);
    std::copy(payload.begin(), payload.end(), frame.begin() + 4);
    return frame;
}
}  // namespace mmltk::testsupport
