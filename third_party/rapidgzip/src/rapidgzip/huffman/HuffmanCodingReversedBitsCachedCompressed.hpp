#pragma once

#include <array>
#include <cassert>
#include <core/BitManipulation.hpp>
#include <cstdint>
#include <utility>

#include "HuffmanCodingReversedBitsCachedBase.hpp"

namespace rapidgzip {
/** Cache slot layout that packs the code length and the symbol into one Symbol-sized value. */
template <uint8_t MAX_CODE_LENGTH, typename Symbol, size_t MAX_SYMBOL_COUNT>
class ReversedBitsPackedCodeCache {
   public:
    using CacheEntry = std::pair<uint8_t, Symbol>;

    static constexpr auto LENGTH_SHIFT = requiredBits(MAX_SYMBOL_COUNT);
    static_assert(MAX_SYMBOL_COUNT <= (1UL << LENGTH_SHIFT), "Not enough free bits to pack length into Symbol!");
    static_assert(LENGTH_SHIFT + requiredBits(MAX_CODE_LENGTH) <= sizeof(Symbol) * 8U,
                  "Symbol type cannot pack both the symbol and code length!");

    constexpr void clear(const uint8_t maxCodeLength) noexcept {
        for (size_t symbol = 0; symbol < (1ULL << maxCodeLength); ++symbol) {
            m_codeCache[symbol] = 0;
        }
    }

    constexpr void store(const std::size_t paddedCode, const uint8_t length, const std::size_t symbol) {
        assert(paddedCode < m_codeCache.size());
        const auto value = static_cast<Symbol>(symbol | static_cast<Symbol>(length << LENGTH_SHIFT));
        assert((value >> LENGTH_SHIFT) == length);
        assert((value & nLowestBitsSet<Symbol, LENGTH_SHIFT>()) == symbol);
        m_codeCache[paddedCode] = value;
    }

    [[nodiscard]] forceinline constexpr CacheEntry entry(const std::size_t paddedCode) const {
        assert(paddedCode < m_codeCache.size());
        auto symbol = m_codeCache[paddedCode];
        const auto length = symbol >> LENGTH_SHIFT;
        symbol &= nLowestBitsSet<Symbol, LENGTH_SHIFT>();
        return {static_cast<uint8_t>(length), symbol};
    }

   private:
    alignas(8) std::array<Symbol, (1UL << MAX_CODE_LENGTH)> m_codeCache{};
};

template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol, size_t MAX_SYMBOL_COUNT>
using HuffmanCodingReversedBitsCachedCompressed =
    HuffmanCodingReversedBitsCachedBase<ReversedBitsPackedCodeCache<MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>,
                                        HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
}  // namespace rapidgzip
