#pragma once

#include <array>
#include <core/VectorView.hpp>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rapidgzip::SimpleRunLengthEncoding {
void writeVarInt(std::vector<uint8_t>& target, uint64_t value) {
    do {
        target.push_back(((value >> 7U) > 0) ? (value | 0b1000'0000ULL) : (value & 0b0111'1111ULL));
        value >>= 7U;
    } while (value > 0);
}

template <typename Container>
[[nodiscard]] constexpr std::pair<uint64_t, uint8_t> readVarInt(const Container& source, size_t offset = 0) {
    uint64_t value{0};
    uint8_t nBytesRead{0};
    for (size_t i = offset; i < source.size(); ++i) {
        const auto byte = source[i];
        if ((nBytesRead == 9) && (byte > 1)) {
            return {0, 0};
        }
        value += static_cast<uint64_t>(byte & 0b0111'1111ULL) << (7U * nBytesRead);
        ++nBytesRead;
        if ((byte & 0b1000'0000U) == 0) {
            return {value, nBytesRead};
        }
    }
    return {0, 0};
}

template <typename Container>
[[nodiscard]] constexpr std::pair<size_t, size_t> findRun(const Container& data, size_t offset = 0,
                                                          size_t minLength = 4) {
    for (; offset < data.size(); ++offset) {
        size_t length = 1;
        while ((offset + length < data.size()) && (data[offset + length] == data[offset])) {
            ++length;
        }
        if (length >= minLength) {
            return {offset, length};
        }
    }
    return {offset, 0};
}

[[nodiscard]] std::vector<uint8_t> simpleRunLengthEncode(const VectorView<uint8_t> data) {
    std::vector<uint8_t> encoded;

    if (data.empty()) {
        return encoded;
    }

    size_t i{0};
    while (i < data.size()) {
        const auto [runOffset, runLength] = findRun(data, i, 6);

        writeVarInt(encoded, 0);
        const auto literalCount = std::min(data.size() - i, runOffset + 1 - i);
        writeVarInt(encoded, literalCount);
        for (size_t j = 0; j < literalCount; ++j) {
            encoded.push_back(data[i + j]);
        }
        i += literalCount;

        if (i >= data.size()) {
            break;
        }
        if (runLength <= 1) {
            continue;
        }

        writeVarInt(encoded, 1);
        writeVarInt(encoded, runLength - 1);
        i += runLength - 1;
    }

    return encoded;
}

template <typename OutputContainer, typename InputContainer>
[[nodiscard]] constexpr OutputContainer simpleRunLengthDecode(const InputContainer& data, size_t decompressedSize) {
    OutputContainer output{};

    if constexpr (std::is_same_v<OutputContainer, std::vector<uint8_t> >) {
        output.resize(decompressedSize, 0);
    } else {
        if ((decompressedSize > 0) && (output.size() != decompressedSize)) {
            throw std::logic_error("Requested decompressed size does not match container!");
        }
    }

    size_t decodedSize = 0;
    size_t i = 0;

    while (i < data.size()) {
        auto [backwardReference, nBytesRead] = readVarInt(data, i);
        if (nBytesRead == 0) {
            throw std::domain_error("Partial varint read for operation type!");
        }
        i += nBytesRead;

        if (backwardReference > decodedSize) {
            throw std::domain_error("Backreference points past the file start!");
        }

        auto [length, nBytesRead2] = readVarInt(data, i);
        if (nBytesRead2 == 0) {
            throw std::domain_error("Partial varint read for literal count/match length!");
        }
        i += nBytesRead2;

        switch (backwardReference) {
            case 0: {
                if (i + length > data.size()) {
                    throw std::domain_error("Literal count points past the end!");
                }

                for (size_t j = 0; (j < length) && (decodedSize + j < output.size()); ++j) {
                    output[decodedSize + j] = data[i + j];
                }
                i += length;
                decodedSize += length;

                break;
            }
            case 1: {
                const auto symbol = decodedSize - backwardReference < output.size()
                                        ? output[decodedSize - backwardReference]
                                        : uint8_t(0);
                if (symbol != 0) {
                    for (size_t j = 0; (j < length) && (decodedSize + j < output.size()); ++j) {
                        output[decodedSize + j] = symbol;
                    }
                }
                decodedSize += length;

                break;
            }
            default:
                throw std::domain_error("Unsupported backward reference!");
        }
    }

    if (decodedSize != output.size()) {
        throw std::logic_error("Decompressed size (" + std::to_string(decodedSize) + ") does not match container (" +
                               std::to_string(output.size()) + ")!");
    }

    return output;
}
}
