#pragma once

#include <array>
#include <cstdint>
#include <filereader/FileReader.hpp>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "Interface.hpp"

namespace rapidgzip::blockfinder {
class Bgzf final : public Interface {
   public:
    using HeaderBytes = std::array<uint8_t, 18>;
    using FooterBytes = std::array<uint8_t, 28>;

    static constexpr FooterBytes BGZF_FOOTER = {0x1F, 0x8B, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
                                                0x06, 0x00, 0x42, 0x43, 0x02, 0x00, 0x1B, 0x00, 0x03, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

   public:
    explicit Bgzf(UniqueFileReader fileReader)
        : m_fileReader(std::move(fileReader)), m_currentBlockOffset(m_fileReader->tell()) {
        const auto header = tryReadHeader(*m_fileReader);
        if (!header) {
            throw std::invalid_argument("Could not read enough data from given file!");
        }

        if (!isBgzfHeader(*header)) {
            throw std::invalid_argument("Given file does not start with a BGZF header!");
        }

        if (m_fileReader->seekable() && m_fileReader->size().has_value()) {
            const auto footer = tryReadFooter(*m_fileReader);
            if (!footer) {
                throw std::invalid_argument("Could not read enough data from given file for BGZF footer!");
            }

            if (*footer != BGZF_FOOTER) {
                throw std::invalid_argument("Given file does not end with a BGZF footer!");
            }

            m_fileReader->seekTo(m_currentBlockOffset);
        }
    }

    /** Reads the leading BGZF header bytes from the current position; nullopt when too little data could be read. */
    [[nodiscard]] static std::optional<HeaderBytes> tryReadHeader(FileReader& file) {
        HeaderBytes header;
        if (file.read(reinterpret_cast<char*>(header.data()), header.size()) != header.size()) {
            return std::nullopt;
        }
        return header;
    }

    /** Seeks to the file end and reads the trailing footer bytes; nullopt when too little data could be read. */
    [[nodiscard]] static std::optional<FooterBytes> tryReadFooter(FileReader& file) {
        FooterBytes footer;
        file.seek(-static_cast<long long int>(footer.size()), SEEK_END);
        if (file.read(reinterpret_cast<char*>(footer.data()), footer.size()) != footer.size()) {
            return std::nullopt;
        }
        return footer;
    }

    [[nodiscard]] static bool isBgzfFile(const UniqueFileReader& file) {
        const auto oldPos = file->tell();
        const auto finishWithResult = [&file, oldPos](const bool isBgzf) {
            file->seekTo(oldPos);
            return isBgzf;
        };

        const auto header = tryReadHeader(*file);
        if (!header || !isBgzfHeader(*header)) {
            return finishWithResult(false);
        }

        if (file->seekable() && file->size().has_value()) {
            const auto footer = tryReadFooter(*file);
            if (!footer || (*footer != BGZF_FOOTER)) {
                return finishWithResult(false);
            }
        }

        return finishWithResult(true);
    }

    [[nodiscard]] static bool isBgzfHeader(const HeaderBytes& header) {
        return (header[0] == 0x1F) && (header[1] == 0x8B) && (header[2] == 0x08) && ((header[3] & (1U << 2U)) != 0) &&
               (header[10] == 0x06) && (header[11] == 0x00) && (header[12] == 'B') && (header[13] == 'C') &&
               (header[14] == 0x02) && (header[15] == 0x00);
    }

    [[nodiscard]] static std::optional<uint16_t> getBgzfCompressedSize(const HeaderBytes& header) {
        if (isBgzfHeader(header)) {
            return (static_cast<uint16_t>(header[17]) << 8U) + header[16];
        }
        return std::nullopt;
    }

    [[nodiscard]] size_t find() override {
        if (m_currentBlockOffset == std::numeric_limits<size_t>::max()) {
            return m_currentBlockOffset;
        }

        auto result = (m_currentBlockOffset + HeaderBytes().size()) * 8;

        m_fileReader->seekTo(m_currentBlockOffset);
        HeaderBytes header;
        const auto nBytesRead = m_fileReader->read(reinterpret_cast<char*>(header.data()), header.size());
        if (nBytesRead == header.size()) {
            const auto blockSize = getBgzfCompressedSize(header);
            if (blockSize) {
                m_currentBlockOffset += *blockSize + 1;
                const auto fileSize = m_fileReader->size();
                if (fileSize && (m_currentBlockOffset >= *fileSize)) {
                    m_currentBlockOffset = std::numeric_limits<size_t>::max();
                }
            } else {
                if (!m_fileReader->eof()) {
                    std::cerr << "Ignoring all junk data after invalid block offset " << m_currentBlockOffset
                              << " B!\n";
                }
                std::cerr << "Failed to get Bgzf metadata!\n";
                m_currentBlockOffset = std::numeric_limits<size_t>::max();
            }
        } else {
            if (nBytesRead > 0) {
                std::cerr << "Got partial header!\n";
            }
            m_currentBlockOffset = std::numeric_limits<size_t>::max();
        }

        return result;
    }

   private:
    const UniqueFileReader m_fileReader;
    size_t m_currentBlockOffset = 0;
};
}
