#include "src/backend/data/benchmark_hash.h"

#include <array>
#include <cstddef>
#include <cstring>

#if defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace mmltk::backend::data {

namespace {

#if !defined(__x86_64__) || !defined(__SSE4_2__)
[[nodiscard]] const std::array<std::uint32_t, 256>& crc32c_table() noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0U; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) != 0U ? 0x82F63B78U : 0U);
            }
            values[index] = value;
        }
        return values;
    }();
    return table;
}
#endif

}  // namespace

std::uint32_t crc32c(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t checksum = 0xFFFFFFFFU;
#if defined(__x86_64__) && defined(__SSE4_2__)
    const std::uint8_t* cursor = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining >= sizeof(std::uint64_t)) {
        std::uint64_t word = 0U;
        std::memcpy(&word, cursor, sizeof(word));
        checksum = static_cast<std::uint32_t>(_mm_crc32_u64(checksum, word));
        cursor += sizeof(word);
        remaining -= sizeof(word);
    }
    while (remaining != 0U) {
        checksum = _mm_crc32_u8(checksum, *cursor);
        ++cursor;
        --remaining;
    }
#else
    const auto& table = crc32c_table();
    for (const std::uint8_t byte : bytes) {
        checksum = table[(checksum ^ byte) & 0xFFU] ^ (checksum >> 8U);
    }
#endif
    return ~checksum;
}

}  // namespace mmltk::backend::data
