#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <utility>

#include "HuffmanCodingReversedBitsCachedBase.hpp"

namespace rapidgzip {
/** Cache slot layout that keeps the code length and the symbol side by side. */
template <uint8_t MAX_CODE_LENGTH, typename Symbol>
class ReversedBitsPairCodeCache {
   public:
    using CacheEntry = std::pair<uint8_t, Symbol>;

    constexpr void clear([[maybe_unused]] const uint8_t maxCodeLength) noexcept {
        for (auto& cached : m_codeCache) {
            cached.first = 0U;
        }
    }

    constexpr void store(const std::size_t paddedCode, const uint8_t length, const std::size_t symbol) {
        assert(paddedCode < m_codeCache.size());
        m_codeCache[paddedCode] = {length, static_cast<Symbol>(symbol)};
    }

    [[nodiscard]] forceinline constexpr CacheEntry entry(const std::size_t paddedCode) const {
        assert(paddedCode < m_codeCache.size());
        return m_codeCache[paddedCode];
    }

   private:
    alignas(8) std::array<CacheEntry, (1UL << MAX_CODE_LENGTH)> m_codeCache{};
};

template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol, size_t MAX_SYMBOL_COUNT>
using HuffmanCodingReversedBitsCached =
    HuffmanCodingReversedBitsCachedBase<ReversedBitsPairCodeCache<MAX_CODE_LENGTH, Symbol>, HuffmanCode,
                                        MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
}  // namespace rapidgzip
