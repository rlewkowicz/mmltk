/**
 * Modified version of bzcat.c part of toybox commit 7bf68329eb3b
 * by Rob Landley released under SPDX-0BSD license.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <core/VectorView.hpp>
#include <cstring>
#include <filereader/BitReader.hpp>
#include <huffman/HuffmanCodingShortBitsCached.hpp>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace bzip2 {
using CRC32LookupTable = std::array<uint32_t, 256>;

[[nodiscard]] constexpr CRC32LookupTable createCRC32LookupTable() noexcept {
    constexpr auto littleEndian{false};
    CRC32LookupTable table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t c = littleEndian ? i : i << 24U;
        for (int j = 0; j < 8; ++j) {
            if (littleEndian) {
                c = ((c & 1U) != 0U) ? (c >> 1U) ^ 0xEDB8'8320U : c >> 1U;
            } else {
                c = (((c & 0x8000'0000U) != 0U)) ? (c << 1U) ^ 0x04C1'1DB7U : (c << 1U);
            }
        }
        table[i] = c;
    }
    return table;
}

static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;

alignas(64) constexpr static CRC32LookupTable CRC32_TABLE = createCRC32LookupTable();

[[nodiscard]] constexpr uint32_t updateCRC32(uint32_t crc, uint8_t data) noexcept {
    return (crc << 8U) ^ CRC32_TABLE[((crc >> 24U) ^ data) & 0xFFU];
}

static constexpr uint32_t MAX_GROUPS = 6;
static constexpr int GROUP_SIZE = 50;
static constexpr int MAX_HUFCODE_BITS = 20;
static constexpr size_t MAX_SYMBOLS = 258;
static constexpr uint16_t SYMBOL_RUNA = 0;
static constexpr uint16_t SYMBOL_RUNB = 1;

constexpr auto MAGIC_BITS_BLOCK = 0x314159265359ULL;
constexpr auto MAGIC_BITS_EOS = 0x177245385090ULL;
constexpr auto MAGIC_BITS_SIZE = 48;
constexpr std::string_view MAGIC_BYTES_BZ2 = "BZh";

using BitReader = rapidgzip::BitReader<true, uint64_t>;

inline uint8_t readBzip2Header(BitReader& bitReader) {
    for (const auto magicByte : MAGIC_BYTES_BZ2) {
        const auto readByte = static_cast<char>(static_cast<uint8_t>(bitReader.read<8>()));
        if (readByte != magicByte) {
            std::stringstream msg;
            msg << "Input header is not BZip2 magic string 'BZh' (0x" << std::hex << int('B') << int('Z') << int('h')
                << std::dec << "). Mismatch at bit position " << bitReader.tell() - 8 << " with " << readByte << " (0x"
                << std::hex << static_cast<int>(static_cast<uint8_t>(readByte)) << ") should be " << magicByte;
            throw std::domain_error(std::move(msg).str());
        }
    }

    const auto i = static_cast<char>(static_cast<uint8_t>(bitReader.read<8>()));
    if ((i < '1') || (i > '9')) {
        std::stringstream msg;
        msg << "Blocksize must be one of '0' (" << std::hex << static_cast<int>('0') << ") ... '9' ("
            << static_cast<int>('9') << ") but is " << i << " (" << static_cast<int>(i) << ")";
        throw std::domain_error(std::move(msg).str());
    }

    return static_cast<uint8_t>(i - '0');
}

struct Block  // NOLINT(clang-analyzer-optin.performance.Padding)
{
   public:
    struct Statistics {
       public:
        void merge(const Statistics& other) {
            durations.merge(other.durations);
        }

       public:
        struct Durations {
           public:
            void merge(const Durations& other) {
                readBlockHeader += other.readBlockHeader;
                decodeBlock += other.decodeBlock;
                readSymbolMaps += other.readSymbolMaps;
                readSelectors += other.readSelectors;
                readTrees += other.readTrees;
                createHuffmanTable += other.createHuffmanTable;
                burrowsWheelerPreparation += other.burrowsWheelerPreparation;
            }

           public:
            double readBlockHeader{0};
            double decodeBlock{0};

            double readSymbolMaps{0};
            double readSelectors{0};
            double readTrees{0};
            double createHuffmanTable{0};
            double burrowsWheelerPreparation{0};
        };

        Durations durations;
    };

    using HuffmanCoding =
        rapidgzip::HuffmanCodingShortBitsCached<uint32_t, MAX_HUFCODE_BITS, uint16_t, MAX_SYMBOLS, 12, false, false>;

   public:
    Block() = default;

    ~Block() = default;

    Block(const Block&) = delete;

    Block& operator=(const Block&) = delete;

    Block(Block&&) = default;

    Block& operator=(Block&&) = default;

    explicit Block(BitReader& bitReader) : m_bitReader(&bitReader) {
        readBlockHeader();
    }

    void readBlockData();

    [[nodiscard]] constexpr bool eos() const noexcept {
        return m_atEndOfStream;
    }

    [[nodiscard]] constexpr bool eob() const noexcept {
        return eos() || !bwdata.hasData();
    }

    [[nodiscard]] constexpr bool eof() const noexcept {
        return m_atEndOfFile;
    }

    [[nodiscard]] BitReader& bitReader() {
        if (m_bitReader != nullptr) {
            return *m_bitReader;
        }
        throw std::invalid_argument("Block has not been initialized yet!");
    }

    [[nodiscard]] size_t read(const size_t nMaxBytesToDecode, char* outputBuffer) {
        const auto t0 = rapidgzip::now();
        const auto result = bwdata.decodeBlock(nMaxBytesToDecode, outputBuffer);
        statistics.durations.decodeBlock += rapidgzip::duration(t0);
        return result;
    }

    [[nodiscard]] constexpr uint32_t dataCRC() const noexcept {
        return bwdata.dataCRC;
    }

    [[nodiscard]] constexpr uint32_t headerCRC() const noexcept {
        return bwdata.headerCRC;
    }

   private:
    template <uint8_t nBits>
    [[nodiscard]] uint32_t getBits() {
        return static_cast<uint32_t>(bitReader().read<nBits>());
    }

    [[nodiscard]] uint32_t getBits(uint8_t nBits) {
        return static_cast<uint32_t>(bitReader().read(nBits));
    }

    void readBlockHeader();

    void readBlockTrees() {
        const auto tReadSymbolMaps = rapidgzip::now();
        readSymbolMaps();
        const auto tReadSelectors = rapidgzip::now();
        readSelectors();
        const auto tReadTrees = rapidgzip::now();
        readTrees();

        statistics.durations.readSymbolMaps += rapidgzip::duration(tReadSymbolMaps, tReadSelectors);
        statistics.durations.readSelectors += rapidgzip::duration(tReadSelectors, tReadTrees);
        statistics.durations.readTrees += rapidgzip::duration(tReadTrees);
    }

    void readSymbolMaps();

    void readSelectors();

    void readTrees();

   public:
    struct BurrowsWheelerTransformData {
        friend Block;

       public:
        [[nodiscard]] size_t decodeBlock(size_t nMaxBytesToDecode, char* outputBuffer);

        [[nodiscard]] constexpr bool hasData() const noexcept {
            return (writeCount > 0) || (symbolRepeatCount > 0);
        }

       private:
        void prepare();

       private:
        uint32_t origPtr = 0;
        std::array<uint32_t, 256> byteCount{};

        uint32_t writePos = 0;
        int writeRun = 0;
        uint32_t writeCount = 0;
        int writeCurrent = 0;

        uint8_t symbolToRepeat{0};
        uint8_t symbolRepeatCount{0};

        uint32_t dataCRC = 0xFFFFFFFFL;
        uint32_t headerCRC = 0;

        std::vector<uint32_t> dbuf = std::vector<uint32_t>(900000, 0);
    };

   public:
    Statistics statistics;

    size_t encodedOffsetInBits = 0;
    size_t encodedSizeInBits = 0;

   private:
    uint64_t magicBytes{0};
    bool isRandomized{false};

    std::array<uint8_t, 256> symbolToByte{};
    std::array<uint8_t, 256> mtfSymbol{};
    unsigned int symbolCount{0};
    uint16_t selectorsCount{0};

    std::array<char, 32768> selectors{};
    std::array<HuffmanCoding, MAX_GROUPS> huffmanCodings{};
    uint32_t groupCount = 0;

    BurrowsWheelerTransformData bwdata;

    BitReader* m_bitReader = nullptr;
    bool m_atEndOfStream = false;
    bool m_atEndOfFile = false;
};

inline void Block::readBlockHeader() {
    const auto tReadBlockHeader = rapidgzip::now();

    encodedOffsetInBits = bitReader().tell();
    encodedSizeInBits = 0;

    magicBytes = ((uint64_t)getBits<24>() << 24U) | (uint64_t)getBits<24>();
    bwdata.headerCRC = getBits(32);
    m_atEndOfStream = magicBytes == MAGIC_BITS_EOS;
    if (m_atEndOfStream) {
        const auto nBitsInByte = static_cast<uint8_t>(bitReader().tell() & 7LLU);
        if (nBitsInByte > 0) {
            bitReader().read(uint8_t(8) - nBitsInByte);
        }

        encodedSizeInBits = bitReader().tell() - encodedOffsetInBits;
        m_atEndOfFile = bitReader().eof();
        return;
    }

    if (magicBytes != MAGIC_BITS_BLOCK) {
        std::stringstream msg;
        msg << "[BZip2 block header] invalid compressed magic 0x" << std::hex << magicBytes << " at offset "
            << rapidgzip::formatBits(encodedOffsetInBits);
        throw std::domain_error(std::move(msg).str());
    }

    isRandomized = getBits<1>() != 0;
    if (isRandomized) {
        throw std::domain_error("[BZip2 block header] deprecated isRandomized bit is not supported");
    }

    if ((bwdata.origPtr = getBits<24>()) > bwdata.dbuf.size()) {
        std::stringstream msg;
        msg << "[BZip2 block header] origPtr " << bwdata.origPtr
            << " is larger than buffer size: " << bwdata.dbuf.size();
        throw std::logic_error(std::move(msg).str());
    }

    readBlockTrees();
    statistics.durations.readBlockHeader += rapidgzip::duration(tReadBlockHeader);
}

inline void Block::readSymbolMaps() {
    const uint16_t huffmanUsedMap = getBits<16>();
    symbolCount = 0;
    for (int i = 0; i < 16; i++) {
        if ((huffmanUsedMap & (1U << (15U - i))) != 0) {
            const auto bitmap = getBits<16>();
            for (int j = 0; j < 16; j++) {
                if ((bitmap & (1U << (15U - j))) != 0) {
                    symbolToByte[symbolCount++] = (16 * i) + j;
                }
            }
        }
    }
}

inline void Block::readSelectors() {
    groupCount = getBits<3>();
    if ((groupCount < 2) || (groupCount > MAX_GROUPS)) {
        std::stringstream msg;
        msg << "[BZip2 block header] Invalid Huffman coding group count " << groupCount;
        throw std::logic_error(std::move(msg).str());
    }

    selectorsCount = getBits<15>();
    if (selectorsCount == 0) {
        std::stringstream msg;
        msg << "[BZip2 block header] The number of selectors " << selectorsCount << " is invalid";
        throw std::logic_error(std::move(msg).str());
    }

    static constexpr std::array<uint8_t, (1U << MAX_GROUPS)> BITS_TO_SELECTOR = []() {
        std::array<uint8_t, (1U << MAX_GROUPS)> result{};
        uint8_t lowestBitsSet{0};
        for (uint8_t selector = 0; selector < MAX_GROUPS; ++selector) {
            const auto paddingBitsCount = MAX_GROUPS - selector;
            const auto maxPaddingBits = static_cast<uint8_t>(1U << paddingBitsCount);
            const auto highestBitsSet = static_cast<uint8_t>(lowestBitsSet << (MAX_GROUPS - selector));
            for (uint8_t paddingBits = 0; paddingBits < maxPaddingBits; ++paddingBits) {
                result[highestBitsSet | paddingBits] = selector;
            }

            lowestBitsSet <<= 1U;
            lowestBitsSet |= 1U;
        }
        result.back() = MAX_GROUPS;
        return result;
    }();

    std::iota(mtfSymbol.begin(), mtfSymbol.begin() + groupCount, 0);
    for (size_t i = 0; i < selectorsCount; i++) {
        const auto j = BITS_TO_SELECTOR.at(m_bitReader->peek<MAX_GROUPS>());
        m_bitReader->seekAfterPeek(j + 1);
        if (j >= groupCount) {
            std::stringstream msg;
            msg << "[BZip2 block header] Could not find zero termination after " << groupCount << " bits";
            throw std::domain_error(std::move(msg).str());
        }

        const auto uc = mtfSymbol[j];
        memmove(mtfSymbol.data() + 1, mtfSymbol.data(), j);
        mtfSymbol[0] = uc;
        selectors[i] = static_cast<char>(uc);
    }
}

inline void Block::readTrees() {
    const auto symCount = symbolCount + 2;
    for (size_t j = 0; j < groupCount; j++) {
        std::array<uint8_t, MAX_SYMBOLS> lengths{};
        unsigned int hh = getBits<5>();
        for (unsigned int symbol = 0; symbol < symCount; symbol++) {
            while (true) {
                if (MAX_HUFCODE_BITS - 1 < hh - 1) {
                    std::stringstream msg;
                    msg << "[BZip2 block header] start_huffman_length " << hh << " is larger than " << MAX_HUFCODE_BITS
                        << " or zero\n";
                    throw std::logic_error(std::move(msg).str());
                }

                if (getBits<1>() != 0) {
                    hh += 1 - (getBits<1>() << 1U);
                } else {
                    break;
                }
            }
            if (hh > std::numeric_limits<uint8_t>::max()) {
                std::stringstream msg;
                msg << "[BZip2 block header] The read code length is unexpectedly large: " << hh;
                throw std::logic_error(std::move(msg).str());
            }
            lengths[symbol] = static_cast<uint8_t>(hh);
        }

        const auto error =
            huffmanCodings[j].initializeFromLengths(rapidgzip::VectorView<uint8_t>(lengths.data(), symCount));
        if (error != rapidgzip::Error::NONE) {
            throw std::domain_error(toString(error));
        }
    }
}

inline void Block::readBlockData() {
    bwdata.byteCount.fill(0);
    std::iota(mtfSymbol.begin(), mtfSymbol.end(), 0);

    const auto t0 = rapidgzip::now();
    uint32_t dbufCount = 0;
    const auto* huffmanCoding = &huffmanCodings.front();
    for (uint32_t hh = 0, runPos = 0, symCount = 0, selector = 0;;) {
        if (symCount-- == 0) {
            symCount = GROUP_SIZE - 1;
            if (selector >= selectorsCount) {
                std::stringstream msg;
                msg << "[BZip2 block data] selector " << selector << " out of maximum range " << selectorsCount;
                throw std::domain_error(std::move(msg).str());
            }
            huffmanCoding = &huffmanCodings[selectors[selector]];
            selector++;
        }

        const auto nextSym = huffmanCoding->decode(*m_bitReader).value();

        if (nextSym <= SYMBOL_RUNB) {
            if (runPos == 0) {
                runPos = 1;
                hh = 0;
            }

            hh += runPos << nextSym;
            runPos <<= 1U;
            continue;
        }

        if (runPos != 0) {
            runPos = 0;
            if (dbufCount + hh > bwdata.dbuf.size()) {
                std::stringstream msg;
                msg << "[BZip2 block data] dbufCount + hh " << dbufCount + hh << " > " << bwdata.dbuf.size()
                    << " dbufSize";
                throw std::domain_error(std::move(msg).str());
            }

            const auto uc = symbolToByte[mtfSymbol[0]];
            bwdata.byteCount[uc] += hh;
            while (hh-- != 0) {
                bwdata.dbuf[dbufCount++] = uc;
            }
        }

        if (nextSym > symbolCount) {
            break;
        }

        if (dbufCount >= bwdata.dbuf.size()) {
            std::stringstream msg;
            msg << "[BZip2 block data] dbufCount " << dbufCount << " > " << bwdata.dbuf.size() << " dbufSize";
            throw std::domain_error(std::move(msg).str());
        }
        const int ii = nextSym - 1;
        auto uc = mtfSymbol[ii];
        std::memmove(mtfSymbol.data() + 1, mtfSymbol.data(), ii);
        mtfSymbol[0] = uc;
        uc = symbolToByte[uc];

        bwdata.byteCount[uc]++;
        bwdata.dbuf[dbufCount++] = uc;
    }

    bwdata.writeCount = dbufCount;
    if (bwdata.origPtr >= dbufCount) {
        std::stringstream msg;
        msg << "[BZip2 block data] origPtr error " << bwdata.origPtr;
        throw std::domain_error(std::move(msg).str());
    }

    statistics.durations.createHuffmanTable += rapidgzip::duration(t0);

    const auto tPrepareStart = rapidgzip::now();
    bwdata.prepare();
    statistics.durations.burrowsWheelerPreparation += rapidgzip::duration(tPrepareStart);

    encodedSizeInBits = bitReader().tell() - encodedOffsetInBits;
}

inline void Block::BurrowsWheelerTransformData::prepare() {
    for (size_t i = 0, cumulativeCount = 0; i < byteCount.size(); ++i) {
        const auto newCumulativeCount = cumulativeCount + byteCount[i];
        byteCount[i] = static_cast<uint32_t>(cumulativeCount);
        cumulativeCount = newCumulativeCount;
    }

    for (uint32_t i = 0; i < writeCount; i++) {
        const auto uc = static_cast<uint8_t>(dbuf[i]);
        dbuf[byteCount[uc]] |= i << 8U;
        byteCount[uc]++;
    }

    dataCRC = 0xFFFFFFFFL;

    if (writeCount > 0) {
        writePos = dbuf[origPtr];
        writeCurrent = static_cast<uint8_t>(writePos & 0xFFU);
        writePos >>= 8U;
        writeRun = -1;
    }

    symbolRepeatCount = 0;
}

inline size_t Block::BurrowsWheelerTransformData::decodeBlock(const size_t nMaxBytesToDecode, char* outputBuffer) {
    if ((outputBuffer == nullptr) || !hasData()) {
        return 0;
    }

    size_t nBytesDecoded = 0;

    const auto writeRepeatedSymbols = [&, this]() {
        while ((symbolRepeatCount > 0) && (nBytesDecoded < nMaxBytesToDecode)) {
            --symbolRepeatCount;
            outputBuffer[nBytesDecoded++] = static_cast<char>(symbolToRepeat);
            dataCRC = updateCRC32(dataCRC, symbolToRepeat);
        }
    };

    writeRepeatedSymbols();

    while ((writeCount > 0) && (nBytesDecoded < nMaxBytesToDecode)) {
        writeCount--;

        const auto previous = writeCurrent;
        writePos = dbuf[writePos];
        writeCurrent = static_cast<uint8_t>(writePos & 0xFFU);
        writePos >>= 8U;

        if (writeRun < 3) {
            outputBuffer[nBytesDecoded++] = static_cast<char>(writeCurrent);
            dataCRC = updateCRC32(dataCRC, writeCurrent);
            if (writeCurrent != previous) {
                writeRun = 0;
            } else {
                ++writeRun;
            }
        } else {
            symbolToRepeat = previous;
            symbolRepeatCount = writeCurrent;
            writeRepeatedSymbols();
            writeCurrent = -1;
            writeRun = 0;
        }
    }

    if ((writeCount == 0) && (symbolRepeatCount == 0)) {
        dataCRC = ~dataCRC;
        if (dataCRC != headerCRC) {
            std::stringstream msg;
            msg << "Calculated CRC " << std::hex << dataCRC << " for block mismatches " << headerCRC;
            throw std::runtime_error(std::move(msg).str());
        }
    }

    return nBytesDecoded;
}
}
