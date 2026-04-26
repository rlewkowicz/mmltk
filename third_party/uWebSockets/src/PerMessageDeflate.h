/*
 * Authored by Alex Hultman, 2018-2021.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_PERMESSAGEDEFLATE_H
#define UWS_PERMESSAGEDEFLATE_H

#include <cstdint>
#include <cstring>

namespace uWS {
enum CompressOptions : uint16_t {
    _COMPRESSOR_MASK = 0x00FF,
    _DECOMPRESSOR_MASK = 0x0F00,
    DISABLED = 0,
    SHARED_COMPRESSOR = 1,
    SHARED_DECOMPRESSOR = 1 << 8,
    DEDICATED_DECOMPRESSOR_32KB = 15 << 8,
    DEDICATED_DECOMPRESSOR_16KB = 14 << 8,
    DEDICATED_DECOMPRESSOR_8KB = 13 << 8,
    DEDICATED_DECOMPRESSOR_4KB = 12 << 8,
    DEDICATED_DECOMPRESSOR_2KB = 11 << 8,
    DEDICATED_DECOMPRESSOR_1KB = 10 << 8,
    DEDICATED_DECOMPRESSOR_512B = 9 << 8,
    DEDICATED_DECOMPRESSOR = 15 << 8,

    DEDICATED_COMPRESSOR_3KB = 9 << 4 | 1,
    DEDICATED_COMPRESSOR_4KB = 9 << 4 | 2,
    DEDICATED_COMPRESSOR_8KB = 10 << 4 | 3,
    DEDICATED_COMPRESSOR_16KB = 11 << 4 | 4,
    DEDICATED_COMPRESSOR_32KB = 12 << 4 | 5,
    DEDICATED_COMPRESSOR_64KB = 13 << 4 | 6,
    DEDICATED_COMPRESSOR_128KB = 14 << 4 | 7,
    DEDICATED_COMPRESSOR_256KB = 15 << 4 | 8,
    DEDICATED_COMPRESSOR = 15 << 4 | 8
};
}  // namespace uWS

#if !defined(UWS_NO_ZLIB) && !defined(UWS_MOCK_ZLIB)
#include <zlib.h>
#endif

#include <string>
#include <optional>

#ifdef UWS_USE_LIBDEFLATE
#include "libdeflate.h"
#include <cstring>
#endif

namespace uWS {

#if defined(UWS_NO_ZLIB) || defined(UWS_MOCK_ZLIB)
struct ZlibContext {};
struct InflationStream {
    std::optional<std::string_view> inflate(ZlibContext*, std::string_view compressed, size_t maxPayloadLength, bool) {
        return compressed.substr(0, std::min(maxPayloadLength, compressed.length()));
    }
    InflationStream(CompressOptions) {}
};
struct DeflationStream {
    std::string_view deflate(ZlibContext*, std::string_view raw, bool) {
        return raw;
    }
    DeflationStream(CompressOptions) {}
};
#else

#define LARGE_BUFFER_SIZE 1024 * 16  // todo: fix this

struct ZlibContext {
    std::string dynamicDeflationBuffer;
    std::string dynamicInflationBuffer;
    char* deflationBuffer;
    char* inflationBuffer;

#ifdef UWS_USE_LIBDEFLATE
    libdeflate_decompressor* decompressor;
    libdeflate_compressor* compressor;
#endif

    ZlibContext() {
        deflationBuffer = (char*)malloc(LARGE_BUFFER_SIZE);
        inflationBuffer = (char*)malloc(LARGE_BUFFER_SIZE);

#ifdef UWS_USE_LIBDEFLATE
        decompressor = libdeflate_alloc_decompressor();
        compressor = libdeflate_alloc_compressor(6);
#endif
    }

    ~ZlibContext() {
        free(deflationBuffer);
        free(inflationBuffer);

#ifdef UWS_USE_LIBDEFLATE
        libdeflate_free_decompressor(decompressor);
        libdeflate_free_compressor(compressor);
#endif
    }
};

struct DeflationStream {
    z_stream deflationStream = {};

    DeflationStream(CompressOptions compressOptions) {
        int windowBits = -(int)((compressOptions & _COMPRESSOR_MASK) >> 4), memLevel = compressOptions & 0xF;

        deflateInit2(&deflationStream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, memLevel, Z_DEFAULT_STRATEGY);
    }

    std::string_view deflate(ZlibContext* zlibContext, std::string_view raw, bool reset) {
        zlibContext->dynamicDeflationBuffer.clear();

        deflationStream.next_in = (Bytef*)raw.data();
        deflationStream.avail_in = (unsigned int)raw.length();

        const int DEFLATE_OUTPUT_CHUNK = LARGE_BUFFER_SIZE;

        int err;
        do {
            deflationStream.next_out = (Bytef*)zlibContext->deflationBuffer;
            deflationStream.avail_out = DEFLATE_OUTPUT_CHUNK;

            err = ::deflate(&deflationStream, Z_SYNC_FLUSH);
            if (Z_OK == err && deflationStream.avail_out == 0) {
                zlibContext->dynamicDeflationBuffer.append(zlibContext->deflationBuffer,
                                                           DEFLATE_OUTPUT_CHUNK - deflationStream.avail_out);
                continue;
            } else {
                break;
            }
        } while (true);

        if (reset) {
            deflateReset(&deflationStream);
        }

        if (zlibContext->dynamicDeflationBuffer.length()) {
            zlibContext->dynamicDeflationBuffer.append(zlibContext->deflationBuffer,
                                                       DEFLATE_OUTPUT_CHUNK - deflationStream.avail_out);

            return std::string_view((char*)zlibContext->dynamicDeflationBuffer.data(),
                                    zlibContext->dynamicDeflationBuffer.length() - 4);
        }

        return {zlibContext->deflationBuffer, DEFLATE_OUTPUT_CHUNK - deflationStream.avail_out - 4};
    }

    ~DeflationStream() {
        deflateEnd(&deflationStream);
    }
};

struct InflationStream {
    z_stream inflationStream = {};

    InflationStream(CompressOptions compressOptions) {
        inflateInit2(&inflationStream, -(compressOptions >> 8));
    }

    ~InflationStream() {
        inflateEnd(&inflationStream);
    }

    std::optional<std::string_view> inflate(ZlibContext* zlibContext, std::string_view compressed,
                                            size_t maxPayloadLength, bool reset) {

#ifdef UWS_USE_LIBDEFLATE
        if (reset) {
            size_t written = 0, consumed;
            zlibContext->dynamicInflationBuffer.clear();
            zlibContext->dynamicInflationBuffer.reserve(maxPayloadLength);

            ((char*)compressed.data())[0] |= 0x1;
            libdeflate_result res = libdeflate_deflate_decompress_ex(
                zlibContext->decompressor, compressed.data(), compressed.length(),
                zlibContext->dynamicInflationBuffer.data(), maxPayloadLength, &consumed, &written);

            if (res == 0 && (consumed == compressed.length() ||
                             (consumed + 1 == compressed.length() && compressed[consumed] == '\0'))) {
                return std::string_view(zlibContext->dynamicInflationBuffer.data(), written);
            } else {
                ((char*)compressed.data())[0] &= ~0x1;
            }
        }
#endif

        char* tailLocation = (char*)compressed.data() + compressed.length();
        char preTailBytes[4];
        memcpy(preTailBytes, tailLocation, 4);

        unsigned char tail[4] = {0x00, 0x00, 0xff, 0xff};
        memcpy(tailLocation, tail, 4);
        compressed = {compressed.data(), compressed.length() + 4};

        zlibContext->dynamicInflationBuffer.clear();

        inflationStream.next_in = (Bytef*)compressed.data();
        inflationStream.avail_in = (unsigned int)compressed.length();

        int err;
        do {
            inflationStream.next_out = (Bytef*)zlibContext->inflationBuffer;
            inflationStream.avail_out = LARGE_BUFFER_SIZE;

            err = ::inflate(&inflationStream, Z_SYNC_FLUSH);
            if (err == Z_OK && inflationStream.avail_out) {
                break;
            }

            zlibContext->dynamicInflationBuffer.append(zlibContext->inflationBuffer,
                                                       LARGE_BUFFER_SIZE - inflationStream.avail_out);

        } while (inflationStream.avail_out == 0 && zlibContext->dynamicInflationBuffer.length() <= maxPayloadLength);

        if (reset) {
            inflateReset(&inflationStream);
        }

        memcpy(tailLocation, preTailBytes, 4);

        if ((err != Z_BUF_ERROR && err != Z_OK) || zlibContext->dynamicInflationBuffer.length() > maxPayloadLength) {
            return std::nullopt;
        }

        if (zlibContext->dynamicInflationBuffer.length()) {
            zlibContext->dynamicInflationBuffer.append(zlibContext->inflationBuffer,
                                                       LARGE_BUFFER_SIZE - inflationStream.avail_out);

            if (zlibContext->dynamicInflationBuffer.length() > maxPayloadLength) {
                return std::nullopt;
            }

            return std::string_view(zlibContext->dynamicInflationBuffer.data(),
                                    zlibContext->dynamicInflationBuffer.length());
        }

        if ((LARGE_BUFFER_SIZE - inflationStream.avail_out) > maxPayloadLength) {
            return std::nullopt;
        }

        return std::string_view(zlibContext->inflationBuffer, LARGE_BUFFER_SIZE - inflationStream.avail_out);
    }
};

#endif

}  // namespace uWS

#endif  // UWS_PERMESSAGEDEFLATE_H
