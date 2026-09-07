
#ifndef UWS_WEBSOCKETCONTEXT_H
#define UWS_WEBSOCKETCONTEXT_H

#include <type_traits>

#include "WebSocket.h"
#include "WebSocketContextData.h"
#include "WebSocketData.h"
#include "WebSocketProtocol.h"

namespace uWS {

template <bool SSL, bool isServer, typename USERDATA>
struct WebSocketContext {
    template <bool>
    friend struct TemplatedApp;
    template <bool, typename>
    friend struct WebSocketProtocol;

   private:
    WebSocketContext() = delete;

    us_socket_context_t* getSocketContext() {
        return (us_socket_context_t*)this;
    }

    WebSocketContextData<SSL, USERDATA>* getExt() {
        return (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(SSL, (us_socket_context_t*)this);
    }

    static bool setCompressed(WebSocketState<isServer>*, void* s) noexcept {
        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)s);

        if (webSocketData->compressionStatus == WebSocketData::CompressionStatus::ENABLED) {
            webSocketData->compressionStatus = WebSocketData::CompressionStatus::COMPRESSED_FRAME;
            return true;
        } else {
            return false;
        }
    }

    static void forceClose(WebSocketState<isServer>*, void* s, std::string_view reason = {}) noexcept {
        us_socket_close(SSL, (us_socket_t*)s, (int)reason.length(), (void*)reason.data());
    }

    static bool handleFragment(char* data, size_t length, unsigned int remainingBytes, int opCode, bool fin,
                               WebSocketState<isServer>* webSocketState, void* s) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(SSL, us_socket_context(SSL, (us_socket_t*)s));
        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)s);
        auto dispatchMessage = [&]() {
            if (opCode == 1 && !protocol::isValidUtf8((unsigned char*)data, length)) {
                forceClose(webSocketState, s, ERR_INVALID_TEXT);
                return true;
            }
            if (webSocketContextData->messageHandler) {
                webSocketContextData->messageHandler((WebSocket<SSL, isServer, USERDATA>*)s,
                                                     std::string_view(data, length), (OpCode)opCode);
                return us_socket_is_closed(SSL, (us_socket_t*)s) || webSocketData->isShuttingDown;
            }
            return false;
        };

        if (opCode < 3) {
            if (!remainingBytes && fin && !webSocketData->fragmentBuffer.length()) {
                if (webSocketData->compressionStatus == WebSocketData::CompressionStatus::COMPRESSED_FRAME) {
                    webSocketData->compressionStatus = WebSocketData::CompressionStatus::ENABLED;

                    LoopData* loopData =
                        (LoopData*)us_loop_ext(us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t*)s)));
                    std::optional<std::string_view> inflatedFrame;
                    if (webSocketData->inflationStream) {
                        inflatedFrame = webSocketData->inflationStream->inflate(
                            loopData->zlibContext, {data, length}, webSocketContextData->maxPayloadLength, false);
                    } else {
                        inflatedFrame = loopData->inflationStream->inflate(
                            loopData->zlibContext, {data, length}, webSocketContextData->maxPayloadLength, true);
                    }

                    if (!inflatedFrame.has_value()) {
                        forceClose(webSocketState, s, ERR_TOO_BIG_MESSAGE_INFLATION);
                        return true;
                    } else {
                        data = (char*)inflatedFrame->data();
                        length = inflatedFrame->length();
                    }
                }

                if (dispatchMessage()) {
                    return true;
                }
            } else {
                if (!webSocketData->fragmentBuffer.length()) {
                    webSocketData->fragmentBuffer.reserve(length + remainingBytes);
                }
                if (refusePayloadLength(length + webSocketData->fragmentBuffer.length(), webSocketState, s)) {
                    forceClose(webSocketState, s, ERR_TOO_BIG_MESSAGE);
                    return true;
                }
                webSocketData->fragmentBuffer.append(data, length);

                if (!remainingBytes && fin) {
                    if (webSocketData->compressionStatus == WebSocketData::CompressionStatus::COMPRESSED_FRAME) {
                        webSocketData->compressionStatus = WebSocketData::CompressionStatus::ENABLED;

                        webSocketData->fragmentBuffer.append("123456789");

                        LoopData* loopData = (LoopData*)us_loop_ext(
                            us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t*)s)));

                        std::optional<std::string_view> inflatedFrame;
                        if (webSocketData->inflationStream) {
                            inflatedFrame = webSocketData->inflationStream->inflate(
                                loopData->zlibContext,
                                {webSocketData->fragmentBuffer.data(), webSocketData->fragmentBuffer.length() - 9},
                                webSocketContextData->maxPayloadLength, false);
                        } else {
                            inflatedFrame = loopData->inflationStream->inflate(
                                loopData->zlibContext,
                                {webSocketData->fragmentBuffer.data(), webSocketData->fragmentBuffer.length() - 9},
                                webSocketContextData->maxPayloadLength, true);
                        }

                        if (!inflatedFrame.has_value()) {
                            forceClose(webSocketState, s, ERR_TOO_BIG_MESSAGE_INFLATION);
                            return true;
                        } else {
                            data = (char*)inflatedFrame->data();
                            length = inflatedFrame->length();
                        }

                    } else {
                        length = webSocketData->fragmentBuffer.length();
                        data = webSocketData->fragmentBuffer.data();
                    }

                    if (dispatchMessage()) {
                        return true;
                    }

                    webSocketData->fragmentBuffer.clear();
                }
            }
        } else {
            WebSocket<SSL, isServer, USERDATA>* webSocket = (WebSocket<SSL, isServer, USERDATA>*)s;

            if (!remainingBytes && fin && !webSocketData->controlTipLength) {
                if (opCode == CLOSE) {
                    auto closeFrame = protocol::parseClosePayload(data, length);
                    webSocket->end(closeFrame.code, std::string_view(closeFrame.message, closeFrame.length));
                    return true;
                } else {
                    if (opCode == PING) {
                        webSocket->send(std::string_view(data, length), (OpCode)OpCode::PONG);
                        if (webSocketContextData->pingHandler) {
                            webSocketContextData->pingHandler(webSocket, {data, length});
                            if (us_socket_is_closed(SSL, (us_socket_t*)s) || webSocketData->isShuttingDown) {
                                return true;
                            }
                        }
                    } else if (opCode == PONG) {
                        if (webSocketContextData->pongHandler) {
                            webSocketContextData->pongHandler(webSocket, {data, length});
                            if (us_socket_is_closed(SSL, (us_socket_t*)s) || webSocketData->isShuttingDown) {
                                return true;
                            }
                        }
                    }
                }
            } else {
                webSocketData->fragmentBuffer.append(data, length);
                webSocketData->controlTipLength += (unsigned int)length;

                if (!remainingBytes && fin) {
                    char* controlBuffer = (char*)webSocketData->fragmentBuffer.data() +
                                          webSocketData->fragmentBuffer.length() - webSocketData->controlTipLength;
                    if (opCode == CLOSE) {
                        protocol::CloseFrame closeFrame =
                            protocol::parseClosePayload(controlBuffer, webSocketData->controlTipLength);
                        webSocket->end(closeFrame.code, std::string_view(closeFrame.message, closeFrame.length));
                        return true;
                    } else {
                        if (opCode == PING) {
                            webSocket->send(std::string_view(controlBuffer, webSocketData->controlTipLength),
                                            (OpCode)OpCode::PONG);
                            if (webSocketContextData->pingHandler) {
                                webSocketContextData->pingHandler(
                                    webSocket, std::string_view(controlBuffer, webSocketData->controlTipLength));
                                if (us_socket_is_closed(SSL, (us_socket_t*)s) || webSocketData->isShuttingDown) {
                                    return true;
                                }
                            }
                        } else if (opCode == PONG) {
                            if (webSocketContextData->pongHandler) {
                                webSocketContextData->pongHandler(
                                    webSocket, std::string_view(controlBuffer, webSocketData->controlTipLength));
                                if (us_socket_is_closed(SSL, (us_socket_t*)s) || webSocketData->isShuttingDown) {
                                    return true;
                                }
                            }
                        }
                    }

                    webSocketData->fragmentBuffer.resize((unsigned int)webSocketData->fragmentBuffer.length() -
                                                         webSocketData->controlTipLength);
                    webSocketData->controlTipLength = 0;
                }
            }
        }
        return false;
    }

    static bool refusePayloadLength(uint64_t length, WebSocketState<isServer>*, void* s) noexcept {
        auto* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(SSL, us_socket_context(SSL, (us_socket_t*)s));

        return webSocketContextData->maxPayloadLength < length;
    }

    WebSocketContext<SSL, isServer, USERDATA>* init() {
        us_socket_context_on_close(SSL, getSocketContext(), [](auto* s, int code, void* reason) noexcept {
            auto* webSocketContextData = (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, us_socket_context(SSL, (us_socket_t*)s));
            auto* ws = (WebSocket<SSL, isServer, USERDATA>*)s;
            const WebSocketData::ConstructionState constructionState = *ws->getConstructionState();
            if (constructionState == WebSocketData::ConstructionState::Raw) {
                return s;
            }
            WebSocketData* webSocketData = (WebSocketData*)(us_socket_ext(SSL, s));
            if (!webSocketData->isShuttingDown &&
                constructionState == WebSocketData::ConstructionState::UserDataConstructed) {
                if (webSocketData->subscriber && webSocketContextData->subscriptionHandler) {
                    try {
                        for (Topic* t : webSocketData->subscriber->topics) {
                            webSocketContextData->subscriptionHandler((WebSocket<SSL, isServer, USERDATA>*)s, t->name,
                                                                      (int)t->size() - 1, (int)t->size());
                        }
                    } catch (...) {
                        webSocketContextData->cDispatchFailureHandler.notify();
                    }
                }

                webSocketContextData->topicTree->freeSubscriber(webSocketData->subscriber);
                webSocketData->subscriber = nullptr;

                if (webSocketContextData->closeHandler) {
                    try {
                        webSocketContextData->closeHandler(ws, 1006, {(char*)reason, (size_t)code});
                    } catch (...) {
                        webSocketContextData->cDispatchFailureHandler.notify();
                    }
                }
                ((USERDATA*)ws->getUserData())->~USERDATA();
                *ws->getConstructionState() = WebSocketData::ConstructionState::WebSocketDataConstructed;
            }

            webSocketData->~WebSocketData();
            *ws->getConstructionState() = WebSocketData::ConstructionState::Raw;
            return s;
        });

        us_socket_context_on_data(SSL, getSocketContext(), [](auto* s, char* data, int length) noexcept {
            WebSocketData* webSocketData = (WebSocketData*)(us_socket_ext(SSL, s));
            if (webSocketData->isShuttingDown) {
                return s;
            }

            auto* webSocketContextData = (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, us_socket_context(SSL, (us_socket_t*)s));
            auto* asyncSocket = (AsyncSocket<SSL>*)s;
            try {
                asyncSocket->timeout(webSocketContextData->idleTimeoutComponents.first);
                webSocketData->hasTimedOut = false;
                asyncSocket->cork();

                WebSocketProtocol<isServer, WebSocketContext<SSL, isServer, USERDATA>>::consume(
                    data, (unsigned int)length, (WebSocketState<isServer>*)webSocketData, s);

                asyncSocket->uncork();
                if (asyncSocket->getBufferedAmount() == 0 && webSocketData->isShuttingDown) {
                    asyncSocket->shutdown();
                }
                return s;
            } catch (...) {
                asyncSocket->uncorkWithoutSending();
                webSocketContextData->cDispatchFailureHandler.notify();
                if (!us_socket_is_closed(SSL, (us_socket_t*)s)) {
                    us_socket_close(SSL, (us_socket_t*)s, 0, nullptr);
                }
                return s;
            }
        });

        us_socket_context_on_writable(SSL, getSocketContext(), [](auto* s) noexcept {
            if (us_socket_is_shut_down(SSL, (us_socket_t*)s)) {
                return s;
            }

            AsyncSocket<SSL>* asyncSocket = (AsyncSocket<SSL>*)s;
            WebSocketData* webSocketData = (WebSocketData*)(us_socket_ext(SSL, s));
            auto* webSocketContextData = (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, us_socket_context(SSL, (us_socket_t*)s));
            try {
                unsigned int backpressure = asyncSocket->getBufferedAmount();
                asyncSocket->write(nullptr, 0);

                if (!backpressure || backpressure > asyncSocket->getBufferedAmount()) {
                    asyncSocket->timeout(webSocketContextData->idleTimeoutComponents.first);
                    webSocketData->hasTimedOut = false;
                }

                if (webSocketData->isShuttingDown) {
                    if (asyncSocket->getBufferedAmount() == 0) {
                        asyncSocket->shutdown();
                    }
                } else if (!backpressure || backpressure > asyncSocket->getBufferedAmount()) {
                    if (webSocketContextData->drainHandler) {
                        webSocketContextData->drainHandler((WebSocket<SSL, isServer, USERDATA>*)s);
                    }
                }
                return s;
            } catch (...) {
                webSocketContextData->cDispatchFailureHandler.notify();
                if (!us_socket_is_closed(SSL, (us_socket_t*)s)) {
                    us_socket_close(SSL, (us_socket_t*)s, 0, nullptr);
                }
                return s;
            }
        });

        us_socket_context_on_end(SSL, getSocketContext(), [](auto* s) noexcept {
            us_socket_close(SSL, (us_socket_t*)s, (int)ERR_TCP_FIN.length(), (void*)ERR_TCP_FIN.data());
            return s;
        });

        us_socket_context_on_long_timeout(SSL, getSocketContext(), [](auto* s) noexcept {
            auto* webSocketContextData = (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, us_socket_context(SSL, (us_socket_t*)s));
            try {
                ((WebSocket<SSL, isServer, USERDATA>*)s)->end(1000, "please reconnect");
                return s;
            } catch (...) {
                webSocketContextData->cDispatchFailureHandler.notify();
                if (!us_socket_is_closed(SSL, (us_socket_t*)s)) {
                    us_socket_close(SSL, (us_socket_t*)s, 0, nullptr);
                }
                return (decltype(s))nullptr;
            }
        });

        us_socket_context_on_timeout(SSL, getSocketContext(), [](auto* s) noexcept {
            auto* webSocketData = (WebSocketData*)(us_socket_ext(SSL, s));
            auto* webSocketContextData = (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, us_socket_context(SSL, (us_socket_t*)s));
            try {
                if (webSocketContextData->sendPingsAutomatically && !webSocketData->isShuttingDown &&
                    !webSocketData->hasTimedOut) {
                    webSocketData->hasTimedOut = true;
                    us_socket_timeout(SSL, s, webSocketContextData->idleTimeoutComponents.second);
                    ((AsyncSocket<SSL>*)s)->write("\x89\x00", 2);
                    return s;
                }

                forceClose(nullptr, s, ERR_WEBSOCKET_TIMEOUT);
                return s;
            } catch (...) {
                webSocketContextData->cDispatchFailureHandler.notify();
                if (!us_socket_is_closed(SSL, (us_socket_t*)s)) {
                    us_socket_close(SSL, (us_socket_t*)s, 0, nullptr);
                }
                return (decltype(s))nullptr;
            }
        });

        return this;
    }

    void free() {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(SSL, (us_socket_context_t*)this);
        webSocketContextData->~WebSocketContextData();

        us_socket_context_free(SSL, (us_socket_context_t*)this);
    }

   public:
    static WebSocketContext* create(Loop*, us_socket_context_t* parentSocketContext,
                                    TopicTree<TopicTreeMessage, TopicTreeBigMessage>* topicTree) {
        static_assert(std::is_nothrow_destructible_v<USERDATA>,
                      "WebSocket user data must be safely destructible from a C callback");
        WebSocketContext* webSocketContext = (WebSocketContext*)us_create_child_socket_context(
            SSL, parentSocketContext, sizeof(WebSocketContextData<SSL, USERDATA>));
        if (!webSocketContext) {
            return nullptr;
        }

        try {
            new ((WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)webSocketContext)) WebSocketContextData<SSL, USERDATA>(topicTree);
        } catch (...) {
            us_socket_context_free(SSL, (us_socket_context_t*)webSocketContext);
            throw;
        }
        return webSocketContext->init();
    }
};

}

#endif  // UWS_WEBSOCKETCONTEXT_H
