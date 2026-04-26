/*
 * Authored by Alex Hultman, 2018-2024.
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

#ifndef UWS_HTTPPARSER_H
#define UWS_HTTPPARSER_H

// todo: HttpParser is in need of a few clean-ups and refactorings

#include <string>
#include <cstring>
#include <algorithm>
#include <climits>
#include <string_view>
#include <map>
#include "MoveOnlyFunction.h"
#include "ChunkedEncoding.h"

#include "BloomFilter.h"
#include "ProxyParser.h"
#include "QueryParser.h"
#include "HttpErrors.h"

namespace uWS {

static const unsigned int MINIMUM_HTTP_POST_PADDING = 32;
static void* FULLPTR = (void*)~(uintptr_t)0;

template <typename T>
std::optional<T*> optional_ptr(T* ptr) {
    return ptr ? std::optional<T*>(ptr) : std::nullopt;
}

static const size_t MAX_FALLBACK_SIZE =
    (size_t)atoi(optional_ptr(getenv("UWS_HTTP_MAX_HEADERS_SIZE")).value_or((char*)"4096"));
#ifndef UWS_HTTP_MAX_HEADERS_COUNT
#define UWS_HTTP_MAX_HEADERS_COUNT 100
#endif

struct HttpRequest {
    friend struct HttpParser;

   private:
    struct Header {
        std::string_view key, value;
    } headers[UWS_HTTP_MAX_HEADERS_COUNT];
    bool ancientHttp;
    unsigned int querySeparator;
    bool didYield;
    BloomFilter bf;
    std::pair<int, std::string_view*> currentParameters;
    std::map<std::string, unsigned short, std::less<>>* currentParameterOffsets = nullptr;

   public:
    bool isAncient() {
        return ancientHttp;
    }

    bool getYield() {
        return didYield;
    }

    struct HeaderIterator {
        Header* ptr;

        bool operator!=(const HeaderIterator& other) const {
            if (ptr != other.ptr) {
                return other.ptr || ptr->key.length();
            }
            return false;
        }

        HeaderIterator& operator++() {
            ptr++;
            return *this;
        }

        std::pair<std::string_view, std::string_view> operator*() const {
            return {ptr->key, ptr->value};
        }
    };

    HeaderIterator begin() {
        return {headers + 1};
    }

    HeaderIterator end() {
        return {nullptr};
    }

    void setYield(bool yield) {
        didYield = yield;
    }

    std::string_view getHeader(std::string_view lowerCasedHeader) {
        if (bf.mightHave(lowerCasedHeader)) {
            for (Header* h = headers; (++h)->key.length();) {
                if (h->key.length() == lowerCasedHeader.length() &&
                    !strncmp(h->key.data(), lowerCasedHeader.data(), lowerCasedHeader.length())) {
                    return h->value;
                }
            }
        }
        return std::string_view(nullptr, 0);
    }

    std::string_view getUrl() {
        return std::string_view(headers->value.data(), querySeparator);
    }

    std::string_view getFullUrl() {
        return std::string_view(headers->value.data(), headers->value.length());
    }

    std::string_view getCaseSensitiveMethod() {
        return std::string_view(headers->key.data(), headers->key.length());
    }

    std::string_view getMethod() {
        /* Compatibility hack: lower case method (todo: remove when major version bumps) */
        for (unsigned int i = 0; i < headers->key.length(); i++) {
            ((char*)headers->key.data())[i] |= 32;
        }

        return std::string_view(headers->key.data(), headers->key.length());
    }

    std::string_view getQuery() {
        if (querySeparator < headers->value.length()) {
            return std::string_view(headers->value.data() + querySeparator + 1,
                                    headers->value.length() - querySeparator - 1);
        } else {
            return std::string_view(nullptr, 0);
        }
    }

    std::string_view getQuery(std::string_view key) {
        std::string_view queryString =
            std::string_view(headers->value.data() + querySeparator, headers->value.length() - querySeparator);

        return getDecodedQueryValue(key, queryString);
    }

    void setParameters(std::pair<int, std::string_view*> parameters) {
        currentParameters = parameters;
    }

    void setParameterOffsets(std::map<std::string, unsigned short, std::less<>>* offsets) {
        currentParameterOffsets = offsets;
    }

    std::string_view getParameter(std::string_view name) {
        if (!currentParameterOffsets) {
            return {nullptr, 0};
        }
        auto it = currentParameterOffsets->find(name);
        if (it == currentParameterOffsets->end()) {
            return {nullptr, 0};
        }
        return getParameter(it->second);
    }

    std::string_view getParameter(unsigned short index) {
        if (currentParameters.first < (int)index) {
            return {};
        } else {
            return currentParameters.second[index];
        }
    }
};

struct HttpParser {
   private:
    std::string fallback;
    uint64_t remainingStreamingBytes = 0;

    static uint64_t toUnsignedInteger(std::string_view str) {
        if (str.length() > 18) {
            return UINT64_MAX;
        }

        uint64_t unsignedIntegerValue = 0;
        for (char c : str) {
            if (c < '0' || c > '9') {
                return UINT64_MAX;
            }
            unsignedIntegerValue = unsignedIntegerValue * 10ull + ((unsigned int)c - (unsigned int)'0');
        }
        return unsignedIntegerValue;
    }

    static inline uint64_t hasLess(uint64_t x, uint64_t n) {
        return (((x) - ~0ULL / 255 * (n)) & ~(x) & ~0ULL / 255 * 128);
    }

    static inline uint64_t hasMore(uint64_t x, uint64_t n) {
        return ((((x) + ~0ULL / 255 * (127 - (n))) | (x)) & ~0ULL / 255 * 128);
    }

    static inline uint64_t hasBetween(uint64_t x, uint64_t m, uint64_t n) {
        return (((~0ULL / 255 * (127 + (n)) - ((x) & ~0ULL / 255 * 127)) & ~(x) &
                 (((x) & ~0ULL / 255 * 127) + ~0ULL / 255 * (127 - (m)))) &
                ~0ULL / 255 * 128);
    }

    static inline bool notFieldNameWord(uint64_t x) {
        return hasLess(x, '-') | hasBetween(x, '-', '0') | hasBetween(x, '9', 'A') | hasBetween(x, 'Z', 'a') |
               hasMore(x, 'z');
    }

    static inline bool isUnlikelyFieldNameByte(unsigned char c) {
        return ((c == '~') | (c == '|') | (c == '`') | (c == '_') | (c == '^') | (c == '.') | (c == '+') | (c == '*') |
                (c == '!')) ||
               ((c >= 48) & (c <= 57)) || ((c <= 39) & (c >= 35));
    }

    static inline bool isFieldNameByteFastLowercased(unsigned char& in) {
        if (((in >= 97) & (in <= 122)) | (in == '-')) [[likely]] {
            return true;
        } else if ((in >= 65) & (in <= 90)) [[unlikely]] {
            in |= 32;
            return true;
        } else if (isUnlikelyFieldNameByte(in)) [[unlikely]] {
            return true;
        }
        return false;
    }

    static inline void* consumeFieldName(char* p) {
        while (true) {
            while ((*p >= 65) & (*p <= 90)) [[likely]] {
                *p |= 32;
                p++;
            }
            while (((*p >= 97) & (*p <= 122))) [[likely]] {
                p++;
            }
            if (*p == ':') {
                return (void*)p;
            }
            if (*p == '-') {
                p++;
            } else if (!((*p >= 65) & (*p <= 90))) {
                break;
            }
        }

        while (isFieldNameByteFastLowercased(*(unsigned char*)p)) {
            p++;
        }
        return (void*)p;
    }

    static inline char* consumeRequestLine(char* data, char* end, HttpRequest::Header& header) {
        char* start = data;
        while (data[0] > 32)
            data++;
        if (&data[1] == end) [[unlikely]] {
            return nullptr;
        }
        if (data[0] == 32 && data[1] == '/') [[likely]] {
            header.key = {start, (size_t)(data - start)};
            data++;
            start = data;
            for (; true; data += 8) {
                uint64_t word;
                memcpy(&word, data, sizeof(uint64_t));
                if (hasLess(word, 33)) {
                    while (*(unsigned char*)data > 32)
                        data++;
                    header.value = {start, (size_t)(data - start)};
                    if (data + 11 >= end) {
                        if (memcmp(" HTTP/1.1\r\n", data, std::min<unsigned int>(11, (unsigned int)(end - data))) ==
                            0) {
                            return nullptr;
                        }
                        return (char*)0x1;
                    }
                    if (memcmp(" HTTP/1.1\r\n", data, 11) == 0) {
                        return data + 11;
                    }
                    if (data[0] == '\r') {
                        return nullptr;
                    }
                    return (char*)0x1;
                }
            }
        }
        if (data[0] == '\r') {
            return nullptr;
        }
        return (char*)0x1;
    }

    static inline void* tryConsumeFieldValue(char* p) {
        for (; true; p += 8) {
            uint64_t word;
            memcpy(&word, p, sizeof(uint64_t));
            if (hasLess(word, 32)) {
                while (*(unsigned char*)p > 31)
                    p++;
                return (void*)p;
            }
        }
    }

    static unsigned int getHeaders(char* postPaddedBuffer, char* end, struct HttpRequest::Header* headers,
                                   void* reserved, unsigned int& err) {
        char *preliminaryKey, *preliminaryValue, *start = postPaddedBuffer;

#ifdef UWS_WITH_PROXY
        ProxyParser* pp = (ProxyParser*)reserved;

        auto [done, offset] = pp->parse({postPaddedBuffer, (size_t)(end - postPaddedBuffer)});
        if (!done) {
            return 0;
        } else {
            postPaddedBuffer += offset;
        }
#else
        (void)reserved;
        (void)end;
#endif

        if ((char*)2 > (postPaddedBuffer = consumeRequestLine(postPaddedBuffer, end, headers[0]))) {
            err = postPaddedBuffer ? HTTP_ERROR_505_HTTP_VERSION_NOT_SUPPORTED : 0;
            return 0;
        }
        headers++;

        for (unsigned int i = 1; i < UWS_HTTP_MAX_HEADERS_COUNT - 1; i++) {
            preliminaryKey = postPaddedBuffer;
            postPaddedBuffer = (char*)consumeFieldName(postPaddedBuffer);
            headers->key = std::string_view(preliminaryKey, (size_t)(postPaddedBuffer - preliminaryKey));

            if (postPaddedBuffer[0] != ':') {
                if (postPaddedBuffer == end) {
                    return 0;
                }
                err = HTTP_ERROR_400_BAD_REQUEST;
                return 0;
            }
            postPaddedBuffer++;

            preliminaryValue = postPaddedBuffer;
            while (true) {
                postPaddedBuffer = (char*)tryConsumeFieldValue(postPaddedBuffer);
                if (postPaddedBuffer[0] != '\r') {
                    if (postPaddedBuffer[0] == '\t') {
                        postPaddedBuffer++;
                        continue;
                    }
                    err = HTTP_ERROR_400_BAD_REQUEST;
                    return 0;
                }
                break;
            }
            if (postPaddedBuffer[1] == '\n') {
                headers->value = std::string_view(preliminaryValue, (size_t)(postPaddedBuffer - preliminaryValue));
                postPaddedBuffer += 2;

                while (headers->value.length() && headers->value.back() < 33) {
                    headers->value.remove_suffix(1);
                }

                while (headers->value.length() && headers->value.front() < 33) {
                    headers->value.remove_prefix(1);
                }

                headers++;

                if (*postPaddedBuffer == '\r') {
                    if (postPaddedBuffer[1] == '\n') {
                        headers->key = std::string_view(nullptr, 0);
                        return (unsigned int)((postPaddedBuffer + 2) - start);
                    } else {
                        if (postPaddedBuffer + 1 < end) {
                            err = HTTP_ERROR_400_BAD_REQUEST;
                        }
                        return 0;
                    }
                }
            } else {
                return 0;
            }
        }
        err = HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE;
        return 0;
    }

    template <int CONSUME_MINIMALLY>
    std::pair<unsigned int, void*> fenceAndConsumePostPadded(
        char* data, unsigned int length, void* user, void* reserved, HttpRequest* req,
        MoveOnlyFunction<void*(void*, HttpRequest*)>& requestHandler,
        MoveOnlyFunction<void*(void*, std::string_view, uint64_t)>& dataHandler) {
        unsigned int consumedTotal = 0;
        unsigned int err = 0;

        data[length] = '\r';
        data[length + 1] = 'a';

        for (unsigned int consumed;
             length && (consumed = getHeaders(data, data + length, req->headers, reserved, err));) {
            data += consumed;
            length -= consumed;
            consumedTotal += consumed;

            if (consumed > MAX_FALLBACK_SIZE) {
                return {HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE, FULLPTR};
            }

            req->ancientHttp = false;

            req->bf.reset();
            for (HttpRequest::Header* h = req->headers; (++h)->key.length();) {
                if (req->bf.mightHave(h->key)) [[unlikely]] {
                    if (h->key == "host" && req->getHeader("host").data()) {
                        return {HTTP_ERROR_400_BAD_REQUEST, FULLPTR};
                    }
                }
                req->bf.add(h->key);
            }

            if (!req->getHeader("host").data()) {
                return {HTTP_ERROR_400_BAD_REQUEST, FULLPTR};
            }

            std::string_view transferEncodingString = req->getHeader("transfer-encoding");
            std::string_view contentLengthString = req->getHeader("content-length");
            if (transferEncodingString.length() && contentLengthString.length()) {
                return {HTTP_ERROR_400_BAD_REQUEST, FULLPTR};
            }

            const char* querySeparatorPtr =
                (const char*)memchr(req->headers->value.data(), '?', req->headers->value.length());
            req->querySeparator =
                (unsigned int)((querySeparatorPtr ? querySeparatorPtr
                                                  : req->headers->value.data() + req->headers->value.length()) -
                               req->headers->value.data());

            void* returnedUser = requestHandler(user, req);
            if (returnedUser != user) {
                return {consumedTotal, returnedUser};
            }

            if (transferEncodingString.length()) {
                remainingStreamingBytes = STATE_IS_CHUNKED;
                if (!CONSUME_MINIMALLY) {
                    /* Go ahead and parse it (todo: better heuristics for emitting FIN to the app level) */
                    std::string_view dataToConsume(data, length);
                    for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                        dataHandler(user, chunk, chunk.length() ? UINT64_MAX : 0);
                    }
                    if (isParsingInvalidChunkedEncoding(remainingStreamingBytes)) {
                        return {HTTP_ERROR_400_BAD_REQUEST, FULLPTR};
                    }
                    unsigned int consumed = (length - (unsigned int)dataToConsume.length());
                    data = (char*)dataToConsume.data();
                    length = (unsigned int)dataToConsume.length();
                    consumedTotal += consumed;
                }
            } else if (contentLengthString.length()) {
                remainingStreamingBytes = toUnsignedInteger(contentLengthString);
                if (remainingStreamingBytes == UINT64_MAX) {
                    return {HTTP_ERROR_400_BAD_REQUEST, FULLPTR};
                }

                if (!CONSUME_MINIMALLY) {
                    unsigned int emittable = (unsigned int)std::min<uint64_t>(remainingStreamingBytes, length);
                    dataHandler(user, std::string_view(data, emittable), remainingStreamingBytes - emittable);
                    remainingStreamingBytes -= emittable;

                    data += emittable;
                    length -= emittable;
                    consumedTotal += emittable;
                }
            } else {
                dataHandler(user, {}, 0);
            }

            if (CONSUME_MINIMALLY) {
                break;
            }
        }
        if (err) {
            return {err, FULLPTR};
        }
        return {consumedTotal, user};
    }

   public:
    std::pair<unsigned int, void*> consumePostPadded(
        char* data, unsigned int length, void* user, void* reserved,
        MoveOnlyFunction<void*(void*, HttpRequest*)>&& requestHandler,
        MoveOnlyFunction<void*(void*, std::string_view, uint64_t)>&& dataHandler) {
        HttpRequest req;
        auto consumeRemainingStreaming = [&]() -> std::pair<bool, std::pair<unsigned int, void*>> {
            if (isParsingChunkedEncoding(remainingStreamingBytes)) {
                std::string_view dataToConsume(data, length);
                for (auto chunk : uWS::ChunkIterator(&dataToConsume, &remainingStreamingBytes)) {
                    dataHandler(user, chunk, chunk.length() ? UINT64_MAX : 0);
                }
                if (isParsingInvalidChunkedEncoding(remainingStreamingBytes)) {
                    return {true, {HTTP_ERROR_400_BAD_REQUEST, FULLPTR}};
                }
                data = (char*)dataToConsume.data();
                length = (unsigned int)dataToConsume.length();
                return {false, {0, user}};
            }
            if (remainingStreamingBytes >= (uint64_t)length) {
                void* returnedUser =
                    dataHandler(user, std::string_view(data, length), remainingStreamingBytes - length);
                remainingStreamingBytes -= length;
                return {true, {0, returnedUser}};
            }
            void* returnedUser = dataHandler(user, std::string_view(data, remainingStreamingBytes), 0);
            data += (unsigned int)remainingStreamingBytes;
            length -= (unsigned int)remainingStreamingBytes;
            remainingStreamingBytes = 0;
            return {returnedUser != user, {0, returnedUser}};
        };

        if (remainingStreamingBytes) {
            auto streamingResult = consumeRemainingStreaming();
            if (streamingResult.first) {
                return streamingResult.second;
            }

        } else if (fallback.length()) {
            unsigned int had = (unsigned int)fallback.length();

            size_t maxCopyDistance = std::min<size_t>(MAX_FALLBACK_SIZE - fallback.length(), (size_t)length);

            fallback.reserve(fallback.length() + maxCopyDistance +
                             std::max<unsigned int>(MINIMUM_HTTP_POST_PADDING, sizeof(std::string)));
            fallback.append(data, maxCopyDistance);

            std::pair<unsigned int, void*> consumed = fenceAndConsumePostPadded<true>(
                fallback.data(), (unsigned int)fallback.length(), user, reserved, &req, requestHandler, dataHandler);
            if (consumed.second != user) {
                return consumed;
            }

            if (consumed.first) {
                fallback.clear();
                data += consumed.first - had;
                length -= consumed.first - had;

                if (remainingStreamingBytes) {
                    auto streamingResult = consumeRemainingStreaming();
                    if (streamingResult.first) {
                        return streamingResult.second;
                    }
                }

            } else {
                if (fallback.length() == MAX_FALLBACK_SIZE) {
                    return {HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE, FULLPTR};
                }
                return {0, user};
            }
        }

        std::pair<unsigned int, void*> consumed =
            fenceAndConsumePostPadded<false>(data, length, user, reserved, &req, requestHandler, dataHandler);
        if (consumed.second != user) {
            return consumed;
        }

        data += consumed.first;
        length -= consumed.first;

        if (length) {
            if (length < MAX_FALLBACK_SIZE) {
                fallback.append(data, length);
            } else {
                return {HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE, FULLPTR};
            }
        }

        return {0, user};
    }
};

}  // namespace uWS

#endif  // UWS_HTTPPARSER_H
