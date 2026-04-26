/*
 * Authored by Alex Hultman, 2018-2025.
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

#ifndef UWS_HTTPRESPONSE_H
#define UWS_HTTPRESPONSE_H

#include "AsyncSocket.h"
#include "HttpResponseData.h"
#include "HttpContext.h"
#include "HttpContextData.h"
#include "Utilities.h"

#include "WebSocketExtensions.h"
#include "WebSocketHandshake.h"
#include "WebSocket.h"
#include "WebSocketContextData.h"

#include "MoveOnlyFunction.h"

/* todo: tryWrite is missing currently, only send smaller segments with write */

namespace uWS {

static const char* HTTP_200_OK = "200 OK";

static const int HTTP_TIMEOUT_S = 10;

template <bool SSL>
struct HttpResponse : public AsyncSocket<SSL> {
    template <bool>
    friend struct TemplatedApp;
    typedef AsyncSocket<SSL> Super;

   private:
    HttpResponseData<SSL>* getHttpResponseData() {
        return (HttpResponseData<SSL>*)Super::getAsyncSocketData();
    }

    void writeUnsignedHex(unsigned int value) {
        char buf[16];
        int length = utils::u32toaHex(value, buf);

        Super::write(buf, length);
    }

    void writeUnsigned64(uint64_t value) {
        char buf[20];
        int length = utils::u64toa(value, buf);

        Super::write(buf, length);
    }

    void writeMark() {
        writeHeader("Date", std::string_view(((LoopData*)us_loop_ext(us_socket_context_loop(
                                                  SSL, (us_socket_context(SSL, (us_socket_t*)this)))))
                                                 ->date,
                                             29));

#ifndef UWS_HTTPRESPONSE_NO_WRITEMARK
        if (!Super::getLoopData()->noMark) {
            writeHeader("uWebSockets", "20");
        }
#endif
    }

    bool internalEnd(std::string_view data, uintmax_t totalSize, bool optional, bool allowContentLength = true,
                     bool closeConnection = false) {
        writeStatus(HTTP_200_OK);

        if (!totalSize) {
            totalSize = data.length();
        }

        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        if (closeConnection) {
            if ((httpResponseData->state & HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE) == 0) {
                writeHeader("Connection", "close");
            }

            httpResponseData->state |= HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE;
        }

        auto markDoneAndCloseIfIdle = [&]() {
            httpResponseData->markDone();
            if (!Super::isCorked() && (httpResponseData->state & HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE) &&
                (httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING) == 0 &&
                ((AsyncSocket<SSL>*)this)->getBufferedAmount() == 0) {
                ((AsyncSocket<SSL>*)this)->shutdown();
                ((AsyncSocket<SSL>*)this)->close();
                return true;
            }
            return false;
        };

        if (httpResponseData->state & HttpResponseData<SSL>::HTTP_WRITE_CALLED) {
            if (data.length()) {
                Super::write("\r\n", 2);
                writeUnsignedHex((unsigned int)data.length());
                Super::write("\r\n", 2);

                Super::write(data.data(), (int)data.length());
            }

            Super::write("\r\n0\r\n\r\n", 7);

            if (markDoneAndCloseIfIdle()) {
                return true;
            }

            Super::timeout(HTTP_TIMEOUT_S);
            return true;
        } else {
            if (!(httpResponseData->state & HttpResponseData<SSL>::HTTP_END_CALLED)) {
                writeMark();

                if (allowContentLength) {
                    Super::write("Content-Length: ", 16);
                    writeUnsigned64(totalSize);
                    Super::write("\r\n\r\n", 4);
                } else {
                    Super::write("\r\n", 2);
                }

                httpResponseData->state |= HttpResponseData<SSL>::HTTP_END_CALLED;
            }

            size_t written = 0;
            bool failed = false;
            while (written < data.length() && !failed) {
                auto writtenFailed = Super::write(data.data() + written,
                                                  (int)std::min<size_t>(data.length() - written, INT_MAX), optional);

                written += (size_t)writtenFailed.first;
                failed = writtenFailed.second;
            }

            httpResponseData->offset += written;

            bool success = written == data.length() && !failed;

            if (!success || httpResponseData->offset == totalSize) {
                Super::timeout(HTTP_TIMEOUT_S);
            }

            if (httpResponseData->offset == totalSize || !data.length()) {
                markDoneAndCloseIfIdle();
            }

            return success;
        }
    }

   public:
#ifdef UWS_WITH_PROXY
    std::string_view getProxiedRemoteAddress() {
        return getHttpResponseData()->proxyParser.getSourceAddress();
    }

    std::string_view getProxiedRemoteAddressAsText() {
        return Super::addressAsText(getProxiedRemoteAddress());
    }

    unsigned int getProxiedRemotePort() {
        return getHttpResponseData()->proxyParser.getSourcePort();
    }
#endif

    /* Manually upgrade to WebSocket. Typically called in upgrade handler. Immediately calls open handler.
     * NOTE: Will invalidate 'this' as socket might change location in memory. Throw away after use. */
    template <typename UserData>
    void upgrade(UserData&& userData, std::string_view secWebSocketKey, std::string_view secWebSocketProtocol,
                 std::string_view secWebSocketExtensions, struct us_socket_context_t* webSocketContext) {
        WebSocketContextData<SSL, UserData>* webSocketContextData =
            (WebSocketContextData<SSL, UserData>*)us_socket_context_ext(SSL, webSocketContext);

        char secWebSocketAccept[29] = {};
        WebSocketHandshake::generate(secWebSocketKey.data(), secWebSocketAccept);

        writeStatus("101 Switching Protocols")
            ->writeHeader("Upgrade", "websocket")
            ->writeHeader("Connection", "Upgrade")
            ->writeHeader("Sec-WebSocket-Accept", secWebSocketAccept);

        if (secWebSocketProtocol.length()) {
            writeHeader("Sec-WebSocket-Protocol", secWebSocketProtocol.substr(0, secWebSocketProtocol.find(',')));
        }

        bool perMessageDeflate = false;
        CompressOptions compressOptions = CompressOptions::DISABLED;
        if (secWebSocketExtensions.length() && webSocketContextData->compression != DISABLED) {
            int wantedInflationWindow = 0;
            if ((webSocketContextData->compression & CompressOptions::_DECOMPRESSOR_MASK) !=
                CompressOptions::SHARED_DECOMPRESSOR) {
                wantedInflationWindow = (webSocketContextData->compression & CompressOptions::_DECOMPRESSOR_MASK) >> 8;
            }

            int wantedCompressionWindow = (webSocketContextData->compression & CompressOptions::_COMPRESSOR_MASK) >> 4;

            auto [negCompression, negCompressionWindow, negInflationWindow, negResponse] =
                negotiateCompression(true, wantedCompressionWindow, wantedInflationWindow, secWebSocketExtensions);

            if (negCompression) {
                perMessageDeflate = true;

                if (negCompressionWindow == 0) {
                    compressOptions = CompressOptions::SHARED_COMPRESSOR;
                } else {
                    compressOptions =
                        (CompressOptions)((uint32_t)(negCompressionWindow << 4) | (uint32_t)(negCompressionWindow - 7));

                    if (webSocketContextData->compression & DEDICATED_COMPRESSOR_3KB) {
                        compressOptions = DEDICATED_COMPRESSOR_3KB;
                    }
                }

                if (negInflationWindow == 0) {
                    compressOptions = CompressOptions(compressOptions | CompressOptions::SHARED_DECOMPRESSOR);
                } else {
                    compressOptions = CompressOptions(compressOptions | (negInflationWindow << 8));
                }

                writeHeader("Sec-WebSocket-Extensions", negResponse);
            }
        }

        internalEnd({nullptr, 0}, 0, false, false);

        HttpContext<SSL>* httpContext = (HttpContext<SSL>*)us_socket_context(SSL, (struct us_socket_t*)this);

        BackPressure backpressure(std::move(((AsyncSocketData<SSL>*)getHttpResponseData())->buffer));

        getHttpResponseData()->~HttpResponseData();

        bool wasCorked = Super::isCorked();

        WebSocket<SSL, true, UserData>* webSocket = (WebSocket<SSL, true, UserData>*)us_socket_context_adopt_socket(
            SSL, (us_socket_context_t*)webSocketContext, (us_socket_t*)this, sizeof(WebSocketData) + sizeof(UserData));

        if (wasCorked) {
            webSocket->AsyncSocket<SSL>::corkUnchecked();
        }

        webSocket->init(perMessageDeflate, compressOptions, std::move(backpressure));

        HttpContextData<SSL>* httpContextData = httpContext->getSocketContextData();
        if (httpContextData->isParsingHttp) {
            httpContextData->upgradedWebSocket = webSocket;
        }

        us_socket_long_timeout(SSL, (us_socket_t*)webSocket, webSocketContextData->maxLifetime);

        us_socket_timeout(SSL, (us_socket_t*)webSocket, webSocketContextData->idleTimeoutComponents.first);

        new (webSocket->getUserData()) UserData(std::move(userData));

        if (webSocketContextData->openHandler) {
            webSocketContextData->openHandler(webSocket);
        }
    }

    using Super::close;

    using Super::getRemoteAddress;
    using Super::getRemoteAddressAsText;
    using Super::getRemotePort;
    using Super::getNativeHandle;

    HttpResponse* pause() {
        Super::pause();
        Super::timeout(0);
        return this;
    }

    HttpResponse* resume() {
        Super::resume();
        Super::timeout(HTTP_TIMEOUT_S);
        return this;
    }

    HttpResponse* writeContinue() {
        Super::write("HTTP/1.1 100 Continue\r\n\r\n", 25);
        return this;
    }

    HttpResponse* writeStatus(std::string_view status) {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        if (httpResponseData->state & HttpResponseData<SSL>::HTTP_STATUS_CALLED) {
            return this;
        }

        httpResponseData->state |= HttpResponseData<SSL>::HTTP_STATUS_CALLED;

        Super::write("HTTP/1.1 ", 9);
        Super::write(status.data(), (int)status.length());
        Super::write("\r\n", 2);
        return this;
    }

    HttpResponse* writeHeader(std::string_view key, std::string_view value) {
        writeStatus(HTTP_200_OK);

        Super::write(key.data(), (int)key.length());
        Super::write(": ", 2);
        Super::write(value.data(), (int)value.length());
        Super::write("\r\n", 2);
        return this;
    }

    HttpResponse* writeHeader(std::string_view key, uint64_t value) {
        writeStatus(HTTP_200_OK);

        Super::write(key.data(), (int)key.length());
        Super::write(": ", 2);
        writeUnsigned64(value);
        Super::write("\r\n", 2);
        return this;
    }

    void endWithoutBody(std::optional<size_t> reportedContentLength = std::nullopt, bool closeConnection = false) {
        if (reportedContentLength.has_value()) {
            internalEnd({nullptr, 0}, reportedContentLength.value(), false, true, closeConnection);
        } else {
            internalEnd({nullptr, 0}, 0, false, false, closeConnection);
        }
    }

    void end(std::string_view data = {}, bool closeConnection = false) {
        internalEnd(data, data.length(), false, true, closeConnection);
    }

    std::pair<bool, bool> tryEnd(std::string_view data, uintmax_t totalSize = 0, bool closeConnection = false) {
        bool ok = internalEnd(data, totalSize, true, true, closeConnection);
        return {ok, hasResponded()};
    }

    bool write(std::string_view data) {
        writeStatus(HTTP_200_OK);

        if (!data.length()) {
            return true;
        }

        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        if (!(httpResponseData->state & HttpResponseData<SSL>::HTTP_WRITE_CALLED)) {
            writeMark();

            writeHeader("Transfer-Encoding", "chunked");
            httpResponseData->state |= HttpResponseData<SSL>::HTTP_WRITE_CALLED;
        }

        Super::write("\r\n", 2);
        writeUnsignedHex((unsigned int)data.length());
        Super::write("\r\n", 2);

        auto [written, failed] = Super::write(data.data(), (int)data.length());
        if (failed) {
            Super::timeout(HTTP_TIMEOUT_S);
        }

        return !failed;
    }

    uintmax_t getWriteOffset() {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        return httpResponseData->offset;
    }

    uint64_t maxRemainingBodyLength() {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        return httpResponseData->maxRemainingBodyLength();
    }

    void overrideWriteOffset(uintmax_t offset) {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        httpResponseData->offset = offset;
    }

    bool hasResponded() {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        return !(httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING);
    }

    HttpResponse* cork(MoveOnlyFunction<void()>&& handler) {
        if (!Super::isCorked() && Super::canCork()) {
            LoopData* loopData = Super::getLoopData();
            Super::cork();
            handler();

            auto* newCorkedSocket = loopData->corkedSocket;

            if (!newCorkedSocket) {
                return this;
            }

            auto [written, failed] = static_cast<Super*>(newCorkedSocket)->uncork();

            if (this != newCorkedSocket) {
                return static_cast<HttpResponse*>(newCorkedSocket);
            }

            if (failed) {
                Super::timeout(HTTP_TIMEOUT_S);
            }

            HttpResponseData<SSL>* httpResponseData = getHttpResponseData();
            if (httpResponseData->state & HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE) {
                if ((httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING) == 0) {
                    if (((AsyncSocket<SSL>*)this)->getBufferedAmount() == 0) {
                        ((AsyncSocket<SSL>*)this)->shutdown();
                        ((AsyncSocket<SSL>*)this)->close();
                    }
                }
            }
        } else {
            handler();
        }

        return this;
    }

    HttpResponse* onWritable(MoveOnlyFunction<bool(uintmax_t)>&& handler) {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        httpResponseData->onWritable = std::move(handler);
        return this;
    }

    HttpResponse* onAborted(MoveOnlyFunction<void()>&& handler) {
        HttpResponseData<SSL>* httpResponseData = getHttpResponseData();

        httpResponseData->onAborted = std::move(handler);
        return this;
    }

    void onData(MoveOnlyFunction<void(std::string_view, bool)>&& handler) {
        if (handler) {
            onDataV2([handler = std::move(handler)](std::string_view chunk, uint64_t maxRemainingBodyLength) mutable {
                handler(chunk, maxRemainingBodyLength == 0);
            });
        } else {
            onDataV2(nullptr);
        }
    }

    void onDataV2(MoveOnlyFunction<void(std::string_view, uint64_t)>&& handler) {
        HttpResponseData<SSL>* data = getHttpResponseData();
        data->inStream = std::move(handler);

        data->received_bytes_per_timeout = 0;
    }
};

}  // namespace uWS

#endif  // UWS_HTTPRESPONSE_H
