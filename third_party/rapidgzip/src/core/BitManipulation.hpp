#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace rapidgzip {
[[nodiscard]] inline bool isLittleEndian() {
    constexpr uint16_t endianTestNumber = 1;
    return *reinterpret_cast<const uint8_t*>(&endianTestNumber) == 1;
}

[[nodiscard]] constexpr uint64_t byteSwap(uint64_t value) {
    value =
        ((value & uint64_t(0x0000'0000'FFFF'FFFFULL)) << 32U) | ((value & uint64_t(0xFFFF'FFFF'0000'0000ULL)) >> 32U);
    value =
        ((value & uint64_t(0x0000'FFFF'0000'FFFFULL)) << 16U) | ((value & uint64_t(0xFFFF'0000'FFFF'0000ULL)) >> 16U);
    value = ((value & uint64_t(0x00FF'00FF'00FF'00FFULL)) << 8U) | ((value & uint64_t(0xFF00'FF00'FF00'FF00ULL)) >> 8U);
    return value;
}

[[nodiscard]] constexpr uint32_t byteSwap(uint32_t value) {
    value = ((value & uint32_t(0x0000'FFFFUL)) << 16U) | ((value & uint32_t(0xFFFF'0000UL)) >> 16U);
    value = ((value & uint32_t(0x00FF'00FFUL)) << 8U) | ((value & uint32_t(0xFF00'FF00UL)) >> 8U);
    return value;
}

[[nodiscard]] constexpr uint16_t byteSwap(uint16_t value) {
    return static_cast<uint16_t>(((static_cast<uint32_t>(value) & 0x00FFU) << 8U) |
                                 ((static_cast<uint32_t>(value) & 0xFF00U) >> 8U));
}

// Shared implementation for nLowestBitsSet / nHighestBitsSet: a mask of
// nBitsSet ones anchored at the least (HIGHEST == false) or most
// (HIGHEST == true) significant bit.
template <typename T, bool HIGHEST>
[[nodiscard]] constexpr T nAnchoredBitsSet(uint8_t nBitsSet) {
    static_assert(std::is_unsigned_v<T>, "Type must be unsigned!");
    if (nBitsSet == 0) {
        return T(0);
    }
    if (nBitsSet >= std::numeric_limits<T>::digits) {
        return static_cast<T>(~T(0));
    }
    const auto nZeroBits = static_cast<uint8_t>(std::max(0, std::numeric_limits<T>::digits - nBitsSet));
    if constexpr (HIGHEST) {
        return static_cast<T>(static_cast<T>(~T(0)) << nZeroBits);
    } else {
        return static_cast<T>(static_cast<T>(~T(0)) >> nZeroBits);
    }
}

template <typename T>
[[nodiscard]] constexpr T nLowestBitsSet(uint8_t nBitsSet) {
    return nAnchoredBitsSet<T, false>(nBitsSet);
}

template <typename T, uint8_t nBitsSet>
[[nodiscard]] constexpr T nLowestBitsSet() {
    return nAnchoredBitsSet<T, false>(nBitsSet);
}

template <typename T>
static constexpr std::array<T, 256U> N_LOWEST_BITS_SET_LUT = []() {
    std::array<T, 256U> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = nLowestBitsSet<T>(static_cast<uint8_t>(i));
    }
    return result;
}();

template <typename T>
[[nodiscard]] constexpr T nHighestBitsSet(uint8_t nBitsSet) {
    return nAnchoredBitsSet<T, true>(nBitsSet);
}

template <typename T>
static constexpr std::array<T, 256U> N_HIGHEST_BITS_SET_LUT = []() {
    std::array<T, 256U> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = nHighestBitsSet<T>(i);
    }
    return result;
}();

template <typename T, uint8_t nBitsSet>
[[nodiscard]] constexpr T nHighestBitsSet() {
    return nAnchoredBitsSet<T, true>(nBitsSet);
}

/**
 * Bit reversal proceeds in stages: stage @p stage swaps adjacent blocks of 2^stage bits.
 * The selection mask for a stage is the repeating pattern of 2^stage ones followed by 2^stage
 * zeros -- 0b0101..., 0b0011'0011..., 0b0000'1111... -- which is exactly
 * max<T> / (2^(2^stage) + 1). Deriving it removes the hand-written per-width mask tables that
 * previously had to be repeated for every integer width.
 */
template <typename T>
[[nodiscard]] constexpr uint64_t reverseBitsBlockMask(const uint8_t stage) {
    static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);
    /* The largest stage for a 64-bit value shifts by 32, so the inner shift never reaches 64. */
    return static_cast<uint64_t>(std::numeric_limits<T>::max()) / ((uint64_t{1} << (uint64_t{1} << stage)) + 1U);
}

template <typename T>
[[nodiscard]] constexpr T reverseBitsWithoutLUT(T data) {
    static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);
    /* One swap stage per halving of the block size: 3 stages for 8 bits, 6 stages for 64 bits.
     * The mask's topmost block is always zero, so the left shift never overflows the width. */
    for (uint8_t stage = 0; (uint64_t{1} << stage) < static_cast<uint64_t>(std::numeric_limits<T>::digits); ++stage) {
        const uint64_t mask = reverseBitsBlockMask<T>(stage);
        const uint64_t shift = uint64_t{1} << stage;
        const auto widened = static_cast<uint64_t>(data);
        data = static_cast<T>(((widened & mask) << shift) | ((widened & ~mask) >> shift));
    }
    return data;
}

[[nodiscard]] constexpr std::array<uint8_t, 1ULL << 8U> createReversedBitsLut8() {
    std::array<uint8_t, 1ULL << 8U> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = reverseBitsWithoutLUT(static_cast<uint8_t>(i));
    }
    return result;
}

alignas(8) inline constexpr auto REVERSED_BITS_LUT_8 = createReversedBitsLut8();

template <typename T>
[[nodiscard]] constexpr T reverseBits(T value) {
    static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);

    if constexpr (sizeof(T) == 1U) {
        return REVERSED_BITS_LUT_8[value];
    } else if constexpr (sizeof(T) == 2U) {
        return static_cast<T>((static_cast<uint16_t>(REVERSED_BITS_LUT_8[value & 0xFFU]) << 8U) |
                              REVERSED_BITS_LUT_8[(value >> 8U) & 0xFFU]);
    } else {
        return reverseBitsWithoutLUT(value);
    }
}

template <typename T>
[[nodiscard]] constexpr T reverseBits(T value, uint8_t bitCount) {
    return reverseBits<T>(value) >> static_cast<uint8_t>(std::numeric_limits<T>::digits - bitCount);
}

[[nodiscard]] constexpr uint8_t requiredBits(const uint64_t stateCount) {
    if (stateCount == 0) {
        return 0;
    }
    if (stateCount == 1) {
        return 1;
    }

    uint8_t result{0};
    for (auto maxValue = stateCount - 1; maxValue != 0; maxValue >>= 1U) {
        ++result;
    }
    return result;
}
}
