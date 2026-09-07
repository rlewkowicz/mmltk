#pragma once

#include <zlib.h>

#include <atomic>
#include <chrono>
#include <core/BlockFetcher.hpp>
#include <core/BlockMap.hpp>
#include <core/FasterVector.hpp>
#include <core/common.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ChunkData.hpp"
#include "GzipBlockFinder.hpp"
#include "WindowMap.hpp"
#include "chunkdecoding/GzipChunk.hpp"

namespace rapidgzip {
template <typename T_FetchingStrategy, typename T_ChunkData = ChunkData>
class GzipChunkFetcher final : public BlockFetcher<GzipBlockFinder, T_ChunkData, T_FetchingStrategy> {
   public:
    using FetchingStrategy = T_FetchingStrategy;
    using ChunkData = T_ChunkData;
    using ChunkConfiguration = typename ChunkData::Configuration;
    using BaseType = BlockFetcher<GzipBlockFinder, ChunkData, FetchingStrategy>;
    using SharedWindow = WindowMap::SharedWindow;
    using SharedDecompressedWindow = std::shared_ptr<const FasterVector<uint8_t> >;
    using WindowView = VectorView<uint8_t>;
    using BlockFinder = typename BaseType::BlockFinder;
    using PostProcessingFutures = std::map<size_t, std::future<void> >;
    using UniqueSharedFileReader = std::unique_ptr<SharedFileReader>;
    using ProcessChunk = std::function<void(const std::shared_ptr<const ChunkData>&, FasterVector<uint8_t>)>;

    static constexpr bool REPLACE_MARKERS_IN_PARALLEL = true;

    struct Statistics : public ChunkData::Statistics {
       public:
        using BaseType = typename ChunkData::Statistics;

       public:
        void merge(const ChunkData& chunkData) {
            const std::scoped_lock lock(mutex);
            BaseType::merge(chunkData.statistics);
            preemptiveStopCount += chunkData.stoppedPreemptively ? 1 : 0;
        }

       public:
        mutable std::mutex mutex;
        uint64_t preemptiveStopCount{0};
        double queuePostProcessingDuration{0};
    };

   public:
    GzipChunkFetcher(UniqueSharedFileReader sharedFileReader, std::shared_ptr<BlockFinder> blockFinder,
                     std::shared_ptr<BlockMap> blockMap, std::shared_ptr<WindowMap> windowMap, size_t parallelization)
        : BaseType(blockFinder, parallelization),
          m_sharedFileReader(std::move(sharedFileReader)),
          m_blockFinder(std::move(blockFinder)),
          m_blockMap(std::move(blockMap)),
          m_windowMap(std::move(windowMap)),
          m_isBgzfFile(m_blockFinder->fileType() == FileType::BGZF) {
        if (!m_sharedFileReader) {
            throw std::invalid_argument("Shared file reader must be valid!");
        }
        if (!m_blockMap) {
            throw std::invalid_argument("Block map must be valid!");
        }
        if (!m_windowMap) {
            throw std::invalid_argument("Window map must be valid!");
        }

        if (m_windowMap->empty()) {
            const auto firstBlockInStream = m_blockFinder->get(0);
            if (!firstBlockInStream) {
                throw std::logic_error("The block finder is required to find the first block itself!");
            }
            m_windowMap->emplace(*firstBlockInStream, {}, CompressionType::NONE);
        }
    }

    ~GzipChunkFetcher() override {
        m_cancelThreads = true;
        this->stopThreadPool();

        if (BaseType::m_showProfileOnDestruction) {
            const auto formatCount = [](const uint64_t count) {
                auto result = std::to_string(count);
                std::string delimited;
                static constexpr size_t DIGIT_GROUP_SIZE = 3;
                delimited.reserve(result.size() + (result.empty() ? 0 : (result.size() - 1) / DIGIT_GROUP_SIZE));
                for (size_t i = 0; i < result.size(); ++i) {
                    const auto distanceFromBack = result.size() - i;
                    if ((i > 0) && (distanceFromBack % 3 == 0)) {
                        delimited.push_back('\'');
                    }
                    delimited.push_back(result[i]);
                }
                return delimited;
            };

            const auto totalDecompressedCount = m_statistics.nonMarkerCount + m_statistics.markerCount;

            std::stringstream out;
            out << std::boolalpha;
            out << "[GzipChunkFetcher::GzipChunkFetcher] First block access statistics:\n";
            out << "    Number of false positives                : " << m_statistics.falsePositiveCount << "\n";
            out << "    Time spent in block finder               : " << m_statistics.blockFinderDuration << " s\n";
            out << "    Time spent decoding with custom inflate  : " << m_statistics.decodeDuration << " s\n";
            out << "    Time spent decoding with inflate wrapper : " << m_statistics.decodeDurationInflateWrapper
                << " s\n";
            out << "    Time spent decoding with ISA-L           : " << m_statistics.decodeDurationIsal << " s\n";
            out << "    Time spent allocating and copying        : " << m_statistics.appendDuration << " s\n";
            out << "    Time spent applying the last window      : " << m_statistics.applyWindowDuration << " s\n";
            out << "    Time spent computing the checksum        : " << m_statistics.computeChecksumDuration << " s\n";
            out << "    Time spent compressing seek points       : " << m_statistics.compressWindowDuration << " s\n";
            out << "    Time spent queuing post-processing       : " << m_statistics.queuePostProcessingDuration
                << " s\n";
            out << "    Total decompressed bytes                 : " << formatCount(totalDecompressedCount) << "\n";
            out << "    Non-marker symbols                       : " << formatCount(m_statistics.nonMarkerCount);
            if (totalDecompressedCount > 0) {
                out << " ("
                    << static_cast<double>(m_statistics.nonMarkerCount) / static_cast<double>(totalDecompressedCount) *
                           100
                    << " %)";
            }
            out << "\n";
            out << "    Replaced marker symbol buffers           : " << formatCount(m_statistics.markerCount);
            if (totalDecompressedCount > 0) {
                out << " ("
                    << static_cast<double>(m_statistics.markerCount) / static_cast<double>(totalDecompressedCount) * 100
                    << " %)";
            }
            out << "\n";
            if (m_statistics.realMarkerCount > 0) {
                out << "    Actual marker symbol count in buffers    : " << formatCount(m_statistics.realMarkerCount);
                if (m_statistics.markerCount > 0) {
                    out << " ("
                        << static_cast<double>(m_statistics.realMarkerCount) /
                               static_cast<double>(m_statistics.markerCount) * 100
                        << " %)";
                }
                out << "\n";
            }
            out << "    Chunks exceeding max. compression ratio  : " << m_statistics.preemptiveStopCount << "\n";

            const auto& fetcherStatistics = BaseType::statistics();
            const auto decodeDuration =
                fetcherStatistics.decodeBlockStartTime && fetcherStatistics.decodeBlockEndTime
                    ? duration(*fetcherStatistics.decodeBlockStartTime, *fetcherStatistics.decodeBlockEndTime)
                    : 0.0;
            const auto optimalDecodeDuration =
                (fetcherStatistics.decodeBlockTotalTime + m_statistics.applyWindowDuration +
                 m_statistics.computeChecksumDuration) /
                fetcherStatistics.parallelization;
            const auto poolEfficiency = optimalDecodeDuration / decodeDuration;

            out << "    Thread Pool Utilization:\n";
            out << "        Total Real Decode Duration    : " << decodeDuration << " s\n";
            out << "        Theoretical Optimal Duration  : " << optimalDecodeDuration << " s\n";
            out << "        Pool Efficiency (Fill Factor) : " << poolEfficiency * 100 << " %\n";
            out << "    BGZF file          : " << m_isBgzfFile << "\n";

            std::cerr << std::move(out).str();
        }
    }

    [[nodiscard]] std::optional<std::pair<size_t, std::shared_ptr<ChunkData> > > get(size_t offset) {
        auto blockInfo = m_blockMap->findDataOffset(offset);
        if (blockInfo.contains(offset)) {
            return getIndexedChunk(offset, blockInfo);
        }

        std::shared_ptr<ChunkData> chunkData;
        for (; !blockInfo.contains(offset); blockInfo = m_blockMap->findDataOffset(offset)) {
            chunkData = processNextChunk();
            if (!chunkData) {
                return std::nullopt;
            }
        }
        return std::make_pair(blockInfo.decodedOffsetInBytes, chunkData);
    }

    void setChunkConfiguration(const ChunkConfiguration configuration) {
        const std::scoped_lock lock{m_chunkConfigurationMutex};
        m_chunkConfiguration = configuration;
    }

    void addChunkIndexingCallback(ProcessChunk processChunk) {
        m_indexFirstSeenChunkCallbacks.emplace_back(std::move(processChunk));
    }

   private:
    [[nodiscard]] std::pair<size_t, std::shared_ptr<ChunkData> > getIndexedChunk(const size_t offset,
                                                                                 const BlockMap::BlockInfo& blockInfo) {
        const auto blockOffset = blockInfo.encodedOffsetInBits;
        if (const auto unsplitBlock = m_unsplitBlocks.find(blockOffset);
            (unsplitBlock != m_unsplitBlocks.end()) && (unsplitBlock->second != blockOffset)) {
            if (const auto chunkData = BaseType::cache().get(unsplitBlock->second); chunkData) {
                auto unsplitBlockInfo = m_blockMap->getEncodedOffset((*chunkData)->encodedOffsetInBits);
                if (unsplitBlockInfo && (blockOffset >= (*chunkData)->encodedOffsetInBits) &&
                    (blockOffset < (*chunkData)->encodedOffsetInBits + (*chunkData)->encodedSizeInBits)) {
                    if ((*chunkData)->containsMarkers()) {
                        std::stringstream message;
                        message << "[GzipChunkFetcher] Did not expect to get results with markers! "
                                << "Requested offset: " << formatBits(offset)
                                << " found to belong to chunk at: " << formatBits(blockOffset)
                                << ", found matching unsplit block with range ["
                                << formatBits((*chunkData)->encodedOffsetInBits) << ", "
                                << formatBits((*chunkData)->encodedOffsetInBits + (*chunkData)->encodedSizeInBits)
                                << "] in the list of " << m_unsplitBlocks.size() << " unsplit blocks.";
                        throw std::logic_error(std::move(message).str());
                    }
                    return std::make_pair(unsplitBlockInfo->decodedOffsetInBytes, *chunkData);
                }
            }
        }

        auto chunkData = getBlock(blockInfo.encodedOffsetInBits, blockInfo.blockIndex);
        if (chunkData && chunkData->containsMarkers()) {
            auto lastWindow = m_windowMap->get(chunkData->encodedOffsetInBits);
            std::stringstream message;
            message << "[GzipChunkFetcher] Did not expect to get results with markers because the offset already "
                    << "exists in the block map!\n"
                    << "    Requested decompressed offset: " << formatBytes(offset)
                    << " found to belong to chunk at: " << formatBits(blockOffset) << " with range ["
                    << formatBits(chunkData->encodedOffsetInBits) << ", "
                    << formatBits(chunkData->encodedOffsetInBits + chunkData->encodedSizeInBits) << "].\n"
                    << "    Window size for the chunk offset: "
                    << (lastWindow ? std::to_string(lastWindow->decompressedSize()) : "no window") << ".";
            throw std::logic_error(std::move(message).str());
        }

        return std::make_pair(blockInfo.decodedOffsetInBytes, std::move(chunkData));
    }

    [[nodiscard]] std::shared_ptr<ChunkData> processNextChunk() {
        if (m_blockMap->finalized()) {
            return {};
        }

        const auto nextBlockOffset = m_blockFinder->get(m_nextUnprocessedBlockIndex);

        if (const auto inputFileSize = m_sharedFileReader->size();
            !nextBlockOffset ||
            (inputFileSize && (*inputFileSize > 0) && (*nextBlockOffset >= *inputFileSize * BYTE_SIZE))) {
            m_blockMap->finalize();
            m_blockFinder->finalize();
            return {};
        }

        auto chunkData = getBlock(*nextBlockOffset, m_nextUnprocessedBlockIndex);

        auto sharedLastWindow = m_windowMap->get(*nextBlockOffset);
        if (!sharedLastWindow) {
            std::stringstream message;
            message << "The window of the last block at " << formatBits(*nextBlockOffset)
                    << " should exist at this point!";
            throw std::logic_error(std::move(message).str());
        }
        const auto lastWindow = sharedLastWindow->decompress();

        postProcessChunk(chunkData, lastWindow);

        chunkData->setEncodedOffset(*nextBlockOffset);
        if (chunkData->encodedSizeInBits == 0) {
            m_blockMap->finalize();
            m_blockFinder->finalize();
            return {};
        }

        appendSubchunksToIndexes(chunkData, chunkData->subchunks(), *lastWindow);

        m_statistics.merge(*chunkData);

        return chunkData;
    }

    void appendSubchunksToIndexes(const std::shared_ptr<const ChunkData>& chunkData,
                                  const std::vector<typename ChunkData::Subchunk>& subchunks,
                                  const FasterVector<uint8_t>& lastWindow) {
        const auto t0 = now();

        for (const auto& subchunk : subchunks) {
            m_blockMap->push(subchunk.encodedOffset, subchunk.encodedSize, subchunk.decodedSize);
            m_blockFinder->insert(subchunk.encodedOffset + subchunk.encodedSize);
        }

        if (subchunks.size() > 1) {
            BaseType::m_fetchingStrategy.splitIndex(m_nextUnprocessedBlockIndex, subchunks.size());

            const auto chunkOffset = chunkData->encodedOffsetInBits;
            const auto partitionOffset = m_blockFinder->partitionOffsetContainingOffset(chunkOffset);
            const auto lookupKey =
                !BaseType::test(chunkOffset) && BaseType::test(partitionOffset) ? partitionOffset : chunkOffset;
            for (const auto& subchunk : subchunks) {
                if (subchunk.encodedOffset != chunkOffset) {
                    m_unsplitBlocks.emplace(subchunk.encodedOffset, lookupKey);
                }
            }
        }

        const auto blockOffsetAfterNext = chunkData->encodedOffsetInBits + chunkData->encodedSizeInBits;

        if (const auto inputFileSize = m_sharedFileReader->size();
            inputFileSize && (*inputFileSize > 0) && (blockOffsetAfterNext >= *inputFileSize * BYTE_SIZE)) {
            m_blockMap->finalize();
            m_blockFinder->finalize();
        }

        m_nextUnprocessedBlockIndex += subchunks.size();
        if (const auto insertedNextBlockOffset = m_blockFinder->get(m_nextUnprocessedBlockIndex);
            !m_blockFinder->finalized() &&
            (!insertedNextBlockOffset.has_value() || (*insertedNextBlockOffset != blockOffsetAfterNext))) {
            std::stringstream message;
            message << "Next block offset index is out of sync! Requested offset to index "
                    << m_nextUnprocessedBlockIndex;
            if (insertedNextBlockOffset.has_value()) {
                message << " and got " << *insertedNextBlockOffset;
            } else {
                message << " and did not get a value";
            }
            message << " but expected " << blockOffsetAfterNext;
            throw std::logic_error(std::move(message).str());
        }

        for (const auto& subchunk : subchunks) {
            const auto windowOffset = subchunk.encodedOffset + subchunk.encodedSize;
            const auto existingWindow = m_windowMap->get(windowOffset);
            if (subchunk.window) {
                if (!existingWindow || !existingWindow->empty()) {
                    m_windowMap->emplaceShared(windowOffset, subchunk.window);
                }
            } else if (!existingWindow) {
                const auto nextDecodedWindowOffset = subchunk.decodedOffset + subchunk.decodedSize;
                m_windowMap->emplace(windowOffset, chunkData->getWindowAt(lastWindow, nextDecodedWindowOffset),
                                     chunkData->windowCompressionType());
                if (BaseType::m_parallelization != 1) {
                    std::stringstream message;
                    message << "[Info] The subchunk window for offset " << formatBits(windowOffset)
                            << " is not compressed yet. Compressing it now might slow down the program.\n";
#ifdef RAPIDGZIP_FATAL_PERFORMANCE_WARNINGS
                    throw std::logic_error(std::move(message).str());
#else
                    std::cerr << std::move(message).str();
#endif
                }
            }
        }

        for (const auto& callback : m_indexFirstSeenChunkCallbacks) {
            callback(chunkData, lastWindow);
        }

        m_statistics.queuePostProcessingDuration += duration(t0);
    }

    void postProcessChunk(const std::shared_ptr<ChunkData>& chunkData, const SharedDecompressedWindow& lastWindow) {
        if constexpr (REPLACE_MARKERS_IN_PARALLEL) {
            waitForReplacedMarkers(chunkData, lastWindow);
        } else {
            chunkData->applyWindow(*lastWindow, chunkData->windowCompressionType());
        }
    }

    void waitForReplacedMarkers(const std::shared_ptr<ChunkData>& chunkData,
                                const SharedDecompressedWindow& lastWindow) {
        using namespace std::chrono_literals;

        auto markerReplaceFuture = m_markersBeingReplaced.find(chunkData->encodedOffsetInBits);
        if ((markerReplaceFuture == m_markersBeingReplaced.end()) && chunkData->hasBeenPostProcessed()) {
            return;
        }

        const auto t0 = now();

        if (markerReplaceFuture == m_markersBeingReplaced.end()) {
            markerReplaceFuture = queueChunkForPostProcessing(chunkData, lastWindow);
        }

        for (auto it = m_markersBeingReplaced.begin(); it != m_markersBeingReplaced.end();) {
            if (it == markerReplaceFuture) {
                ++it;
                continue;
            }

            auto& future = it->second;
            if (!future.valid() || (future.wait_for(0s) == std::future_status::ready)) {
                future.get();
                it = m_markersBeingReplaced.erase(it);
            } else {
                ++it;
            }
        }

        queuePrefetchedChunkPostProcessing();
        m_statistics.queuePostProcessingDuration += duration(t0);

        markerReplaceFuture->second.get();
        m_markersBeingReplaced.erase(markerReplaceFuture);
    }

    void queuePrefetchedChunkPostProcessing() {
        const auto& cacheElements = this->prefetchCache().contents();
        std::vector<size_t> sortedOffsets(cacheElements.size());
        std::transform(cacheElements.begin(), cacheElements.end(), sortedOffsets.begin(),
                       [](const auto& keyValue) { return keyValue.first; });
        std::sort(sortedOffsets.begin(), sortedOffsets.end());
        for (const auto triedStartOffset : sortedOffsets) {
            const auto chunkData = cacheElements.at(triedStartOffset);

            if (m_markersBeingReplaced.find(chunkData->encodedOffsetInBits) != m_markersBeingReplaced.end()) {
                continue;
            }

            if (chunkData->hasBeenPostProcessed()) {
                continue;
            }

            const auto sharedPreviousWindow = m_windowMap->get(chunkData->encodedOffsetInBits);
            if (!sharedPreviousWindow) {
                continue;
            }

            queueChunkForPostProcessing(chunkData, sharedPreviousWindow->decompress());
        }
    }

    PostProcessingFutures::iterator queueChunkForPostProcessing(const std::shared_ptr<ChunkData>& chunkData,
                                                                SharedDecompressedWindow previousWindow) {
        const auto windowOffset = chunkData->encodedOffsetInBits + chunkData->encodedSizeInBits;
        if (!m_windowMap->get(windowOffset)) {
            if (!chunkData->footers.empty() &&
                (chunkData->footers.back().blockBoundary.decodedOffset == chunkData->decodedSizeInBytes)) {
                m_windowMap->emplaceShared(windowOffset, std::make_shared<WindowMap::Window>());
            } else {
                m_windowMap->emplace(windowOffset, chunkData->getLastWindow(*previousWindow), CompressionType::NONE);
            }
        }

        return m_markersBeingReplaced
            .emplace(chunkData->encodedOffsetInBits,
                     this->submitTaskWithHighPriority([chunkData, window = std::move(previousWindow)]() {
                         chunkData->applyWindow(*window, chunkData->windowCompressionType());
                     }))
            .first;
    }

    [[nodiscard]] std::shared_ptr<ChunkData> getBlock(const size_t blockOffset, const size_t blockIndex) {
        const auto getPartitionOffsetFromOffset = [this](auto offset) {
            return m_blockFinder->partitionOffsetContainingOffset(offset);
        };
        const auto partitionOffset = getPartitionOffsetFromOffset(blockOffset);

        std::shared_ptr<ChunkData> chunkData;
        try {
            if (BaseType::test(partitionOffset)) {
                chunkData = BaseType::get(partitionOffset, blockIndex, getPartitionOffsetFromOffset);
            }
        } catch (const NoBlockInRange&) {
        }

        if (BaseType::statisticsEnabled()) {
            if (chunkData && !chunkData->matchesEncodedOffset(blockOffset) && (partitionOffset != blockOffset) &&
                (m_statistics.preemptiveStopCount == 0)) {
                std::stringstream message;
                message << "[Info] Detected a performance problem. Decoding might take longer than necessary. "
                        << "Please consider opening a performance bug report with "
                        << "a reproducing compressed file. Detailed information:\n"
                        << "[Info] Found mismatching block. Need offset " << formatBits(blockOffset)
                        << ". Look in partition offset: " << formatBits(partitionOffset) << ". Found possible range: ["
                        << formatBits(chunkData->encodedOffsetInBits) << ", "
                        << formatBits(chunkData->maxEncodedOffsetInBits) << "]\n";
#ifdef RAPIDGZIP_FATAL_PERFORMANCE_WARNINGS
                throw std::logic_error(std::move(message).str());
#else
                std::cerr << std::move(message).str();
#endif
            }
        }

        if (!chunkData || (!chunkData->matchesEncodedOffset(blockOffset) && (partitionOffset != blockOffset))) {
            try {
                chunkData = BaseType::get(blockOffset, blockIndex, getPartitionOffsetFromOffset);
            } catch (const gzip::BitReader::EndOfFileReached& exception) {
                std::cerr << "Unexpected end of file when getting block at " << formatBits(blockOffset)
                          << " (block index: " << blockIndex << ") on demand\n";
                throw exception;
            }
        }

        if (!chunkData || (chunkData->encodedOffsetInBits == std::numeric_limits<size_t>::max())) {
            std::stringstream message;
            message << "Decoding failed at block offset " << formatBits(blockOffset) << "!";
            throw std::domain_error(std::move(message).str());
        }

        if (!chunkData->matchesEncodedOffset(blockOffset)) {
            std::stringstream message;
            message << "Got wrong block to searched offset! Looked for " << blockOffset
                    << " and looked up cache successively for estimated offset " << partitionOffset
                    << " but got block with actual offset ";
            if (chunkData->encodedOffsetInBits == chunkData->maxEncodedOffsetInBits) {
                message << chunkData->encodedOffsetInBits;
            } else {
                message << "[" << chunkData->encodedOffsetInBits << ", " << chunkData->maxEncodedOffsetInBits << "]";
            }
            throw std::logic_error(std::move(message).str());
        }

        return chunkData;
    }

    [[nodiscard]] ChunkData decodeBlock(size_t blockOffset, size_t nextBlockOffset) const override {
        const auto blockInfo = m_blockMap->getEncodedOffset(blockOffset);

        ChunkConfiguration chunkDataConfiguration;
        {
            const std::scoped_lock lock{m_chunkConfigurationMutex};
            chunkDataConfiguration = m_chunkConfiguration;
        }
        chunkDataConfiguration.fileType = m_blockFinder->fileType();
        chunkDataConfiguration.splitChunkSize = m_blockFinder->spacingInBits() / 8U;

        auto sharedWindow = m_windowMap->get(blockOffset);
        if (!sharedWindow && m_isBgzfFile && !m_blockFinder->finalized()) {
            sharedWindow = std::make_shared<WindowMap::Window>();
        }

        return decodeBlock(
            m_sharedFileReader->clone(), blockOffset,
            (blockInfo ? blockInfo->encodedOffsetInBits + blockInfo->encodedSizeInBits : nextBlockOffset),
            std::move(sharedWindow), blockInfo ? blockInfo->decodedSizeInBytes : std::optional<size_t>{},
            m_cancelThreads, chunkDataConfiguration, m_isBgzfFile || blockInfo);
    }

   public:
    [[nodiscard]] static ChunkData decodeBlock(UniqueFileReader&& sharedFileReader, size_t const blockOffset,
                                               size_t const untilOffset, SharedWindow initialWindow,
                                               std::optional<size_t> const decodedSize,
                                               std::atomic<bool> const& cancelThreads,
                                               ChunkConfiguration const chunkDataConfiguration,
                                               bool const untilOffsetIsExact = false) {
        return GzipChunk<ChunkData>::decodeChunk(std::move(sharedFileReader), blockOffset, untilOffset,
                                                 std::move(initialWindow), decodedSize, cancelThreads,
                                                 chunkDataConfiguration, untilOffsetIsExact);
    }

   private:
    mutable Statistics m_statistics;

    std::atomic<bool> m_cancelThreads{false};

    const UniqueSharedFileReader m_sharedFileReader;
    std::shared_ptr<BlockFinder> const m_blockFinder;
    std::shared_ptr<BlockMap> const m_blockMap;
    std::shared_ptr<WindowMap> const m_windowMap;

    const bool m_isBgzfFile;

    mutable std::mutex m_chunkConfigurationMutex;
    ChunkConfiguration m_chunkConfiguration;

    size_t m_nextUnprocessedBlockIndex{0};

    std::unordered_map<size_t, size_t> m_unsplitBlocks;

    PostProcessingFutures m_markersBeingReplaced;

    std::list<ProcessChunk> m_indexFirstSeenChunkCallbacks;
};
}
