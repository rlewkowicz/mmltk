
#ifndef UWS_WEBSOCKET_H
#define UWS_WEBSOCKET_H

#include "WebSocketData.h"
#include "WebSocketProtocol.h"
#include "AsyncSocket.h"
#include "WebSocketContextData.h"

#include <string_view>

namespace uWS {

enum CompressFlags : int {
    NO_ACTION,
    COMPRESS,
    ALREADY_COMPRESSED
};

template <bool SSL, bool isServer, typename USERDATA>
struct WebSocket : AsyncSocket<SSL> {
    template <bool>
    friend struct TemplatedApp;
    template <bool>
    friend struct HttpResponse;

   private:
    typedef AsyncSocket<SSL> Super;

    void* init(bool perMessageDeflate, CompressOptions compressOptions, BackPressure&& backpressure) {
        new (us_socket_ext(SSL, (us_socket_t*)this))
            WebSocketData(perMessageDeflate, compressOptions, std::move(backpressure));
        return this;
    }

   public:
    USERDATA* getUserData() {
        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);
        return (USERDATA*)(webSocketData + 1);
    }

    using Super::getBufferedAmount;
    using Super::getRemoteAddress;
    using Super::getRemoteAddressAsText;
    using Super::getRemotePort;
    using Super::getNativeHandle;

    us_socket_t* close() {
        if (us_socket_is_closed(SSL, (us_socket_t*)this)) {
            return nullptr;
        }
        WebSocketData* webSocketData = (WebSocketData*)Super::getAsyncSocketData();
        if (webSocketData->isShuttingDown) {
            return nullptr;
        }

        return us_socket_close(SSL, (us_socket_t*)this, 0, nullptr);
    }

    enum SendStatus : int {
        BACKPRESSURE,
        SUCCESS,
        DROPPED
    };

    SendStatus sendFirstFragment(std::string_view message, OpCode opCode = OpCode::BINARY, bool compress = false) {
        return send(message, opCode, compress, false);
    }

    SendStatus sendFragment(std::string_view message, bool compress = false) {
        return send(message, CONTINUATION, compress, false);
    }

    SendStatus sendLastFragment(std::string_view message, bool compress = false) {
        return send(message, CONTINUATION, compress, true);
    }

    bool hasNegotiatedCompression() {
        WebSocketData* webSocketData = (WebSocketData*)Super::getAsyncSocketData();
        return webSocketData->compressionStatus == WebSocketData::ENABLED;
    }

    SendStatus sendPrepared(PreparedMessage& preparedMessage) {
        if (preparedMessage.compressed && hasNegotiatedCompression() &&
            preparedMessage.compressedMessage.length() < preparedMessage.originalMessage.length()) {
            return send({preparedMessage.compressedMessage.data(), preparedMessage.compressedMessage.length()},
                        (OpCode)preparedMessage.opCode, uWS::CompressFlags::ALREADY_COMPRESSED);
        }
        return send({preparedMessage.originalMessage.data(), preparedMessage.originalMessage.length()},
                    (OpCode)preparedMessage.opCode);
    }

    SendStatus send(std::string_view message, OpCode opCode = OpCode::BINARY, int compress = false, bool fin = true) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        if (webSocketContextData->maxBackpressure && webSocketContextData->maxBackpressure < getBufferedAmount()) {
            if (webSocketContextData->closeOnBackpressureLimit) {
                us_socket_shutdown_read(SSL, (us_socket_t*)this);
            }

            if (webSocketContextData->droppedHandler) {
                webSocketContextData->droppedHandler(this, message, opCode);
            }

            return DROPPED;
        }

        WebSocketData* webSocketData = (WebSocketData*)Super::getAsyncSocketData();

        if (message.length() >= 16 * 1024 && !compress && !SSL && !webSocketData->subscriber &&
            getBufferedAmount() == 0 && Super::getLoopData()->corkOffset == 0) {
            char header[10];
            int header_length =
                (int)protocol::formatMessage<isServer>(header, "", 0, opCode, message.length(), compress, fin);
            int written = us_socket_write2(0, (struct us_socket_t*)this, header, header_length, message.data(),
                                           (int)message.length());

            if (written != header_length + (int)message.length()) {
                if (written > header_length) {
                    webSocketData->buffer.append(message.data() + written - header_length,
                                                 message.length() - (size_t)(written - header_length));
                } else {
                    webSocketData->buffer.append(header + written, (size_t)header_length - (size_t)written);
                    webSocketData->buffer.append(message.data(), message.length());
                }
                Super::uncorkWithoutSending();
                return BACKPRESSURE;
            }
        } else {
            if (webSocketData->subscriber) {
                webSocketContextData->topicTree->drain(webSocketData->subscriber);
            }

            if (compress) {
                WebSocketData* webSocketData = (WebSocketData*)Super::getAsyncSocketData();

                if (message.length() && opCode < 3 && webSocketData->compressionStatus == WebSocketData::ENABLED) {
                    if (compress != CompressFlags::ALREADY_COMPRESSED) {
                        LoopData* loopData = Super::getLoopData();
                        if (webSocketData->deflationStream) {
                            message = webSocketData->deflationStream->deflate(loopData->zlibContext, message, false);
                        } else {
                            message = loopData->deflationStream->deflate(loopData->zlibContext, message, true);
                        }
                    }
                } else {
                    compress = false;
                }
            }

            size_t messageFrameSize = protocol::messageFrameSize(message.length());
            auto [sendBuffer, sendBufferAttribute] = Super::getSendBuffer(messageFrameSize);
            protocol::formatMessage<isServer>(sendBuffer, message.data(), message.length(), opCode, message.length(),
                                              compress, fin);

            if (sendBufferAttribute == SendBufferAttribute::NEEDS_DRAIN) {
                auto [written, failed] = Super::write(nullptr, 0);
                if (failed) {
                    return BACKPRESSURE;
                }
            } else if (sendBufferAttribute == SendBufferAttribute::NEEDS_UNCORK) {
                auto [written, failed] = Super::uncork();
                if (failed) {
                    return BACKPRESSURE;
                }
            }
        }

        if (webSocketContextData->resetIdleTimeoutOnSend) {
            Super::timeout(webSocketContextData->idleTimeoutComponents.first);
            WebSocketData* webSocketData = (WebSocketData*)Super::getAsyncSocketData();
            webSocketData->hasTimedOut = false;
        }

        return SUCCESS;
    }

    void end(int code = 0, std::string_view message = {}) {
        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);
        if (webSocketData->isShuttingDown) {
            return;
        }

        webSocketData->isShuttingDown = true;

        static const int MAX_CLOSE_PAYLOAD = 123;
        size_t length = std::min<size_t>(MAX_CLOSE_PAYLOAD, message.length());
        char closePayload[MAX_CLOSE_PAYLOAD + 2];
        size_t closePayloadLength = protocol::formatClosePayload(closePayload, (uint16_t)code, message.data(), length);
        bool ok = send(std::string_view(closePayload, closePayloadLength), OpCode::CLOSE);

        if (!this->isCorked()) {
            if (ok) {
                this->shutdown();
            }
        }

        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        Super::timeout(webSocketContextData->idleTimeoutComponents.second);

        if (webSocketData->subscriber && webSocketContextData->subscriptionHandler) {
            for (Topic* t : webSocketData->subscriber->topics) {
                webSocketContextData->subscriptionHandler(this, t->name, (int)t->size() - 1, (int)t->size());
            }
        }

        webSocketContextData->topicTree->freeSubscriber(webSocketData->subscriber);
        webSocketData->subscriber = nullptr;

        if (webSocketContextData->closeHandler) {
            webSocketContextData->closeHandler(this, code, message);
        }
        ((USERDATA*)this->getUserData())->~USERDATA();
    }

    void cork(MoveOnlyFunction<void()>&& handler) {
        if (!Super::isCorked() && Super::canCork()) {
            Super::cork();
            handler();

            auto [written, failed] = Super::uncork();
            (void)written;
            (void)failed;
        } else {
            handler();
        }
    }

    bool subscribe(std::string_view topic, bool = false) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);
        if (!webSocketData->subscriber) {
            webSocketData->subscriber = webSocketContextData->topicTree->createSubscriber();
            webSocketData->subscriber->user = this;
        }

        Topic* topicOrNull = webSocketContextData->topicTree->subscribe(webSocketData->subscriber, topic);
        if (topicOrNull && webSocketContextData->subscriptionHandler) {
            webSocketContextData->subscriptionHandler(this, topic, (int)topicOrNull->size(),
                                                      (int)topicOrNull->size() - 1);
        }

        return true;
    }

    bool unsubscribe(std::string_view topic, bool = false) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);

        if (!webSocketData->subscriber) {
            return false;
        }

        auto [ok, last, newCount] = webSocketContextData->topicTree->unsubscribe(webSocketData->subscriber, topic);
        if (ok && webSocketContextData->subscriptionHandler) {
            webSocketContextData->subscriptionHandler(this, topic, newCount, newCount + 1);
        }

        return ok;
    }

    bool isSubscribed(std::string_view topic) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);
        if (!webSocketData->subscriber) {
            return false;
        }

        Topic* topicPtr = webSocketContextData->topicTree->lookupTopic(topic);
        if (!topicPtr) {
            return false;
        }

        return topicPtr->count(webSocketData->subscriber);
    }

    void iterateTopics(MoveOnlyFunction<void(std::string_view)> cb) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);
        if (webSocketData->subscriber) {
            webSocketContextData->topicTree->iteratingSubscriber = webSocketData->subscriber;

            for (Topic* topicPtr : webSocketData->subscriber->topics) {
                cb({topicPtr->name.data(), topicPtr->name.length()});
            }

            webSocketContextData->topicTree->iteratingSubscriber = nullptr;
        }
    }

    bool publish(std::string_view topic, std::string_view message, OpCode opCode = OpCode::TEXT,
                 bool compress = false) {
        WebSocketContextData<SSL, USERDATA>* webSocketContextData =
            (WebSocketContextData<SSL, USERDATA>*)us_socket_context_ext(
                SSL, (us_socket_context_t*)us_socket_context(SSL, (us_socket_t*)this));

        WebSocketData* webSocketData = (WebSocketData*)us_socket_ext(SSL, (us_socket_t*)this);
        if (!webSocketData->subscriber) {
            return false;
        }

        if (message.length() >= LoopData::CORK_BUFFER_SIZE) {
            return webSocketContextData->topicTree->publishBig(
                webSocketData->subscriber, topic, {message, opCode, compress},
                [](Subscriber* s, TopicTreeBigMessage& message) {
                    auto* ws = (WebSocket<SSL, true, int>*)s->user;

                    ws->send(message.message, (OpCode)message.opCode, message.compress);
                });
        } else {
            return webSocketContextData->topicTree->publish(webSocketData->subscriber, topic,
                                                            {std::string(message), opCode, compress});
        }
    }
};

}  

#endif  // UWS_WEBSOCKET_H
