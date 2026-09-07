#pragma once

#include <core/BitManipulation.hpp>
#include <core/VectorView.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace rapidgzip::detail {

template <typename HuffmanCode, typename BitCount, typename CodeValues, typename Store>
constexpr void populateReversedBitsCache(const VectorView<BitCount>& codeLengths, const uint8_t minimumCodeLength,
                                         const uint8_t maximumCodeLength, CodeValues codeValues, Store&& store) {
    for (std::size_t symbol = 0U; symbol < codeLengths.size(); ++symbol) {
        const auto length = codeLengths[symbol];
        if (length == 0U) {
            continue;
        }

        const auto code = codeValues[length - minimumCodeLength]++;
        const auto reversedCode = reverseBits(code, length);
        const auto fillerBitCount = static_cast<uint8_t>(maximumCodeLength - length);
        const auto maximumPaddedCode = static_cast<HuffmanCode>(
            reversedCode | static_cast<HuffmanCode>(nLowestBitsSet<HuffmanCode>(fillerBitCount) << length));
        const std::size_t increment = std::size_t{1U} << length;
        for (std::size_t paddedCode = reversedCode; paddedCode <= maximumPaddedCode; paddedCode += increment) {
            std::forward<Store>(store)(paddedCode, length, symbol);
        }
    }
}

}
