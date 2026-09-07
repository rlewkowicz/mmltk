#pragma once

#include <cstdint>
#include <huffman/HuffmanCodingSymbolsPerLength.hpp>
#include <optional>
#include <rapidgzip/gzip/definitions.hpp>
#include <utility>

#include "HuffmanCodingCacheInitialization.hpp"

namespace rapidgzip {
/**
 * Reversed-bits cached Huffman decoder. Initialization fills a lookup table indexed by
 * MAX_CODE_LENGTH reversed bits and decoding resolves a symbol from a single peek into that table.
 *
 * How one cache slot is represented is the only degree of freedom and is supplied by @p CodeCache,
 * which must provide a CacheEntry typedef of (code length, symbol) plus clear(), store() and
 * entry(). HuffmanCodingReversedBitsCached stores the length and the symbol side by side,
 * HuffmanCodingReversedBitsCachedCompressed packs both into the Symbol type to halve the cache
 * footprint; everything else is shared and therefore lives here exactly once.
 */
template <typename CodeCache, typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol, size_t MAX_SYMBOL_COUNT>
class HuffmanCodingReversedBitsCachedBase
    : public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT> {
   public:
    using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;
    using CacheEntry = typename CodeCache::CacheEntry;

    [[nodiscard]] constexpr Error initializeFromLengths(const VectorView<BitCount>& codeLengths) {
        if (const auto errorCode = BaseType::initializeFromLengths(codeLengths); errorCode != Error::NONE) {
            return errorCode;
        }

        if (m_needsToBeZeroed) {
            m_codeCache.clear(this->m_maxCodeLength);
        }

        detail::populateReversedBitsCache<HuffmanCode>(
            codeLengths, this->m_minCodeLength, this->m_maxCodeLength, this->m_minimumCodeValuesPerLevel,
            [this](const std::size_t paddedCode, const uint8_t length, const std::size_t symbol) {
                m_codeCache.store(paddedCode, length, symbol);
            });
        m_needsToBeZeroed = true;
        return Error::NONE;
    }

    [[nodiscard]] forceinline std::optional<Symbol> decode(gzip::BitReader& bitReader) const {
        try {
            const auto value = bitReader.peek(this->m_maxCodeLength);
            const auto [length, symbol] = m_codeCache.entry(static_cast<std::size_t>(value));
            if (length == 0U) {
                return std::nullopt;
            }
            bitReader.seekAfterPeek(length);
            return symbol;
        } catch (const gzip::BitReader::EndOfFileReached&) {
            return BaseType::decode(bitReader);
        }
    }

   private:
    CodeCache m_codeCache{};
    bool m_needsToBeZeroed{false};
};
}  // namespace rapidgzip
