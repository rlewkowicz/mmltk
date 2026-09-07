#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <huffman/HuffmanCodingSymbolsPerLength.hpp>
#include <limits>
#include <optional>
#include <rapidgzip/gzip/definitions.hpp>

namespace rapidgzip {
template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol, size_t MAX_SYMBOL_COUNT>
class HuffmanCodingReversedCodesPerLength
    : public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT> {
   public:
    using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

   protected:
    constexpr void initializeCodingTable(const VectorView<BitCount>& codeLengths,
                                         const CodeLengthFrequencies& bitLengthFrequencies) {
        size_t sum = 0;
        for (uint8_t bitLength = this->m_minCodeLength; bitLength <= this->m_maxCodeLength; ++bitLength) {
            this->m_offsets[bitLength - this->m_minCodeLength] = static_cast<uint16_t>(sum);
            sum += bitLengthFrequencies[bitLength];
        }
        this->m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1] = static_cast<uint16_t>(sum);

        assert(sum <= this->m_symbolsPerLength.size() && "Specified max symbol range exceeded!");

        auto sizes = this->m_offsets;
        auto codeValuesPerLevel = this->m_minimumCodeValuesPerLevel;
        for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
            const auto length = codeLengths[symbol];
            if (length != 0) {
                const auto k = length - this->m_minCodeLength;
                const auto code = codeValuesPerLevel[k];
                codeValuesPerLevel[k]++;

                this->m_symbolsPerLength[sizes[k]] = static_cast<Symbol>(symbol);
                m_codesPerLength[sizes[k]] = reverseBits(code, length);
                sizes[k]++;
            }
        }
    }

   public:
    [[nodiscard]] constexpr Error initializeFromLengths(const VectorView<BitCount>& codeLengths) {
        return this->initializeFromLengthsWithTable(codeLengths, [this](const auto& lengths, const auto& frequencies) {
            initializeCodingTable(lengths, frequencies);
        });
    }

    [[nodiscard]] forceinline std::optional<Symbol> decode(gzip::BitReader& bitReader) const {
        HuffmanCode code = bitReader.read(this->m_minCodeLength);

        const auto size = this->m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1];
        auto relativeCodeLength = 0;
        for (size_t i = 0; i < size; ++i) {
            if (m_codesPerLength[i] == code) {
                return this->m_symbolsPerLength[i];
            }

            while (this->m_offsets[relativeCodeLength + 1] == i + 1) {
                code |= bitReader.read<1>() << (this->m_minCodeLength + relativeCodeLength);
                relativeCodeLength++;
            }
        }

        return std::nullopt;
    }

   protected:
    alignas(8) std::array<HuffmanCode, MAX_SYMBOL_COUNT> m_codesPerLength{};
};
}
