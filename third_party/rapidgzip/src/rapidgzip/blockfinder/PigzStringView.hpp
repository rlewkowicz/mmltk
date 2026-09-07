#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <core/common.hpp>
#include <cstdint>
#include <filereader/Buffered.hpp>
#include <filereader/FileReader.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <rapidgzip/gzip/definitions.hpp>
#include <rapidgzip/gzip/gzip.hpp>
#include <string_view>
#include <utility>
#include <vector>

#include "Interface.hpp"

namespace rapidgzip::blockfinder {
class PigzStringView final : public Interface {
   public:
    static constexpr size_t BUFFER_SIZE = 16_Ki;
    static constexpr uint8_t MAGIC_BIT_STRING_SIZE = 35;

   public:
    explicit PigzStringView(UniqueFileReader fileReader)
        : m_fileReader(std::move(fileReader)), m_fileSize(m_fileReader->size()) {}

    [[nodiscard]] size_t find() override {
        while (m_blockOffsets.empty() && !m_fileReader->eof() && !m_fileReader->fail() && !m_fileReader->closed()) {
            if (foundFirstBlock) {
                analyzeNextChunk();
            } else {
                findFirstBlock();
            }
        }

        if (m_blockOffsets.empty()) {
            return std::numeric_limits<std::size_t>::max();
        }

        m_lastReturnedBlockOffset = m_blockOffsets.back() * CHAR_BIT;
        m_blockOffsets.pop_back();
        return m_lastReturnedBlockOffset;
    }

   private:
    void findBlockOffsets(const std::string_view& stringView, std::size_t offset) {
        for (auto position = stringView.find(EMPTY_DEFLATE_BLOCK.data(), 0, EMPTY_DEFLATE_BLOCK.size());
             position != std::string_view::npos;
             position = stringView.find(EMPTY_DEFLATE_BLOCK.data(), position + 1, EMPTY_DEFLATE_BLOCK.size())) {
            if ((position >= 1) && ((static_cast<uint8_t>(stringView[position - 1]) & 0b1110'0000) == 0)) {
                const auto totalOffset = offset + position + EMPTY_DEFLATE_BLOCK.size();
                auto fileSize = m_fileSize ? m_fileSize : m_fileReader->size();
                if (!fileSize || (totalOffset < *fileSize)) {
                    m_blockOffsets.push_back(totalOffset);
                }
            }
        }
    }

    void analyzeNextChunk() {
        constexpr std::size_t nBytesToRetain = ceilDiv(MAGIC_BIT_STRING_SIZE, CHAR_BIT) - 1;
        static_assert(nBytesToRetain == 4, "Assuming bit string size of 35 for empty deflate block.");
        const auto checkBoundary = m_bufferSize > 0;
        std::array<char, 2 * nBytesToRetain> boundaryBuffer{};
        std::size_t boundaryBufferSize = 0;

        if (checkBoundary) {
            boundaryBufferSize = std::min(m_bufferSize, nBytesToRetain);
            for (std::size_t i = 0; i < boundaryBufferSize; ++i) {
                boundaryBuffer[i] = m_buffer[i + (m_bufferSize - nBytesToRetain)];
            }
        }

        const auto bufferOffset = m_fileReader->tell();
        const auto boundaryBufferOffset = bufferOffset - boundaryBufferSize;
        m_bufferSize = m_fileReader->read(m_buffer.data(), BUFFER_SIZE);

        if (checkBoundary) {
            boundaryBufferSize += std::min(nBytesToRetain, m_bufferSize);
            for (std::size_t i = 0; i < std::min(nBytesToRetain, m_bufferSize); ++i) {
                boundaryBuffer[nBytesToRetain + i] = m_buffer[i];
            }

            findBlockOffsets({boundaryBuffer.data(), boundaryBufferSize}, boundaryBufferOffset);
        }

        findBlockOffsets({m_buffer.data(), m_bufferSize}, bufferOffset);
    }

    void findFirstBlock() {
#if 0
        gzip::BitReader bitReader( m_fileReader->clone() );

#else

        BufferedFileReader::AlignedBuffer buffer(BUFFER_SIZE);
        buffer.resize(m_fileReader->read(buffer.data(), buffer.size()));
        gzip::BitReader bitReader(std::make_unique<BufferedFileReader>(std::move(buffer)));

#endif

        if ((rapidgzip::gzip::checkHeader(bitReader) == rapidgzip::Error::NONE) && (bitReader.tell() % CHAR_BIT == 0)) {
            m_blockOffsets.push_back(bitReader.tell() / CHAR_BIT);
            m_fileReader->seekTo(0);
            m_bufferSize = 0;
            foundFirstBlock = true;
            return;
        }

        m_fileReader->seek(0, SEEK_END);
    }

   private:
    const UniqueFileReader m_fileReader;
    const std::optional<std::size_t> m_fileSize;

    alignas(64) std::array<char, BUFFER_SIZE> m_buffer{};
    size_t m_bufferSize{0};

    bool foundFirstBlock{false};
    std::vector<std::size_t> m_blockOffsets;
    std::size_t m_lastReturnedBlockOffset{0};

    static constexpr std::string_view EMPTY_DEFLATE_BLOCK{"\0\0\xFF\xFF", 4};
};
}
