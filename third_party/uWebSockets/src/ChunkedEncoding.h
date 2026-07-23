
#ifndef UWS_CHUNKEDENCODING_H
#define UWS_CHUNKEDENCODING_H

#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string_view>
#include "MoveOnlyFunction.h"
#include <optional>

namespace uWS {

constexpr uint64_t STATE_HAS_SIZE = 1ull << (sizeof(uint64_t) * 8 - 1);
constexpr uint64_t STATE_IS_CHUNKED = 1ull << (sizeof(uint64_t) * 8 - 2);
constexpr uint64_t STATE_SIZE_MASK = ~(3ull << (sizeof(uint64_t) * 8 - 2));
constexpr uint64_t STATE_IS_ERROR = ~0ull;
constexpr uint64_t STATE_SIZE_OVERFLOW = 0x0Full << (sizeof(uint64_t) * 8 - 8);

inline uint64_t chunkSize(uint64_t state) {
    return state & STATE_SIZE_MASK;
}

inline void consumeHexNumber(std::string_view& data, uint64_t& state) {
    while (data.length() && data.data()[0] > 32) {
        unsigned char digit = (unsigned char)data.data()[0];
        if (digit >= 'a') {
            digit = (unsigned char)(digit - ('a' - ':'));
        } else if (digit >= 'A') {
            digit = (unsigned char)(digit - ('A' - ':'));
        }

        unsigned int number = ((unsigned int)digit - (unsigned int)'0');

        if (number > 16 || (chunkSize(state) & STATE_SIZE_OVERFLOW)) {
            state = STATE_IS_ERROR;
            return;
        }

        uint64_t bits = STATE_IS_CHUNKED;

        state = (state & STATE_SIZE_MASK) * 16ull + number;

        state |= bits;
        data.remove_prefix(1);
    }
    while (data.length() && data.data()[0] != '\n') {
        data.remove_prefix(1);
    }
    if (data.length()) {
        state += 2;
        state |= STATE_HAS_SIZE | STATE_IS_CHUNKED;
        data.remove_prefix(1);
    }
}

inline void decChunkSize(uint64_t& state, unsigned int by) {
    state = (state & ~STATE_SIZE_MASK) | (chunkSize(state) - by);
}

inline bool hasChunkSize(uint64_t state) {
    return state & STATE_HAS_SIZE;
}

inline bool isParsingChunkedEncoding(uint64_t state) {
    return state & ~STATE_SIZE_MASK;
}

inline bool isParsingInvalidChunkedEncoding(uint64_t state) {
    return state == STATE_IS_ERROR;
}

static std::optional<std::string_view> getNextChunk(std::string_view& data, uint64_t& state, bool trailer = false) {
    while (data.length()) {
        if (((state & STATE_IS_CHUNKED) == 0) && hasChunkSize(state) && chunkSize(state)) {
            while (data.length() && chunkSize(state)) {
                data.remove_prefix(1);
                decChunkSize(state, 1);

                if (chunkSize(state) == 0) {
                    state = 0;

                    return std::nullopt;
                }
            }
            continue;
        }

        if (!hasChunkSize(state)) {
            consumeHexNumber(data, state);
            if (isParsingInvalidChunkedEncoding(state)) {
                return std::nullopt;
            }
            if (hasChunkSize(state) && chunkSize(state) == 2) {
                if (trailer) {
                    state = 4 | STATE_HAS_SIZE;
                } else {
                    state = 2 | STATE_HAS_SIZE;
                }

                return std::string_view(nullptr, 0);
            }
            continue;
        }

        if (data.length() >= chunkSize(state)) {
            std::string_view emitSoon;
            bool shouldEmit = false;
            if (chunkSize(state) > 2) {
                emitSoon = std::string_view(data.data(), chunkSize(state) - 2);
                shouldEmit = true;
            }
            data.remove_prefix(chunkSize(state));
            state = STATE_IS_CHUNKED;
            if (shouldEmit) {
                return emitSoon;
            }
            continue;
        } else {
            std::string_view emitSoon;
            if (chunkSize(state) > 2) {
                uint64_t maximalAppEmit = chunkSize(state) - 2;
                if (data.length() > maximalAppEmit) {
                    emitSoon = data.substr(0, maximalAppEmit);
                } else {
                    emitSoon = data;
                }
            }
            decChunkSize(state, (unsigned int)data.length());
            state |= STATE_IS_CHUNKED;
            data.remove_prefix(data.length());
            if (emitSoon.length()) {
                return emitSoon;
            } else {
                return std::nullopt;
            }
        }
    }

    return std::nullopt;
}

struct ChunkIterator {
    std::string_view* data;
    std::optional<std::string_view> chunk;
    uint64_t* state;
    bool trailer;

    ChunkIterator(std::string_view* data, uint64_t* state, bool trailer = false)
        : data(data), state(state), trailer(trailer) {
        chunk = uWS::getNextChunk(*data, *state, trailer);
    }

    ChunkIterator() {}

    ChunkIterator begin() {
        return *this;
    }

    ChunkIterator end() {
        return ChunkIterator();
    }

    std::string_view operator*() {
        if (!chunk.has_value()) {
            std::abort();
        }
        return chunk.value();
    }

    bool operator!=(const ChunkIterator& other) const {
        return other.chunk.has_value() != chunk.has_value();
    }

    ChunkIterator& operator++() {
        chunk = uWS::getNextChunk(*data, *state, trailer);
        return *this;
    }
};
}  

#endif  // UWS_CHUNKEDENCODING_H
