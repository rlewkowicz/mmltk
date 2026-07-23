
#ifndef UWS_ASYNCSOCKET_H
#define UWS_ASYNCSOCKET_H

#include <cstring>
#include <iostream>

#include "libusockets.h"

#include "LoopData.h"
#include "AsyncSocketData.h"

namespace uWS {

enum SendBufferAttribute {
    NEEDS_NOTHING,
    NEEDS_DRAIN,
    NEEDS_UNCORK
};

template <bool, bool, typename>
struct WebSocketContext;

template <bool SSL>
struct AsyncSocket {
    template <bool>
    friend struct HttpContext;
    template <bool, bool, typename>
    friend struct WebSocketContext;
    template <bool>
    friend struct TemplatedApp;
    template <bool, typename>
    friend struct WebSocketContextData;
    template <typename, typename>
    friend struct TopicTree;
    template <bool>
    friend struct HttpResponse;

   private:
    void throttle_helper(int toggle) {
        static thread_local int us_events[2] = {0, 0};

        struct us_poll_t* p = (struct us_poll_t*)this;
        struct us_loop_t* loop = us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t*)this));

        if (toggle) {
            int events = us_poll_events(p);
            if (events) {
                us_events[getBufferedAmount() ? 1 : 0] = events;
            }
            us_poll_change(p, loop, 0);
        } else {
            int events = us_events[getBufferedAmount() ? 1 : 0];
            us_poll_change(p, loop, events);
        }
    }

   protected:
    void* getNativeHandle() {
        return us_socket_get_native_handle(SSL, (us_socket_t*)this);
    }

    LoopData* getLoopData() {
        return (LoopData*)us_loop_ext(us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t*)this)));
    }

    AsyncSocketData<SSL>* getAsyncSocketData() {
        return (AsyncSocketData<SSL>*)us_socket_ext(SSL, (us_socket_t*)this);
    }

    void timeout(unsigned int seconds) {
        us_socket_timeout(SSL, (us_socket_t*)this, seconds);
    }

    void shutdown() {
        us_socket_shutdown(SSL, (us_socket_t*)this);
    }

    us_socket_t* pause() {
        throttle_helper(1);
        return (us_socket_t*)this;
    }

    us_socket_t* resume() {
        throttle_helper(0);
        return (us_socket_t*)this;
    }

    us_socket_t* close() {
        return us_socket_close(SSL, (us_socket_t*)this, 0, nullptr);
    }

    void corkUnchecked() {
        getLoopData()->corkedSocket = this;
    }

    void uncorkWithoutSending() {
        if (isCorked()) {
            getLoopData()->corkedSocket = nullptr;
        }
    }

    void cork() {
        if (getLoopData()->corkOffset && getLoopData()->corkedSocket != this) {
            std::cerr << "Error: Cork buffer must not be acquired without checking canCork!" << std::endl;
            std::terminate();
        }

        getLoopData()->corkedSocket = this;
    }

    bool isCorked() {
        return getLoopData()->corkedSocket == this;
    }

    bool canCork() {
        return getLoopData()->corkedSocket == nullptr;
    }

    std::pair<char*, SendBufferAttribute> getSendBuffer(size_t size) {
        LoopData* loopData = getLoopData();
        BackPressure& backPressure = getAsyncSocketData()->buffer;
        size_t existingBackpressure = backPressure.length();
        if ((!existingBackpressure) && (isCorked() || canCork()) &&
            (loopData->corkOffset + size < LoopData::CORK_BUFFER_SIZE)) {
            if (isCorked()) {
                char* sendBuffer = loopData->corkBuffer + loopData->corkOffset;
                loopData->corkOffset += (unsigned int)size;
                return {sendBuffer, SendBufferAttribute::NEEDS_NOTHING};
            } else {
                cork();
                char* sendBuffer = loopData->corkBuffer + loopData->corkOffset;
                loopData->corkOffset += (unsigned int)size;
                return {sendBuffer, SendBufferAttribute::NEEDS_UNCORK};
            }
        } else {
            unsigned int ourCorkOffset = 0;
            if (isCorked() && loopData->corkOffset) {
                ourCorkOffset = loopData->corkOffset;
                loopData->corkOffset = 0;
            }

            backPressure.resize(ourCorkOffset + existingBackpressure + size);

            memcpy((char*)backPressure.data() + existingBackpressure, loopData->corkBuffer, ourCorkOffset);

            return {(char*)backPressure.data() + ourCorkOffset + existingBackpressure,
                    SendBufferAttribute::NEEDS_DRAIN};
        }
    }

    unsigned int getBufferedAmount() {
        return (unsigned int)getAsyncSocketData()->buffer.totalLength();
    }

    std::string_view addressAsText(std::string_view binary) {
        static thread_local char buf[64];
        int ipLength = 0;

        if (!binary.length()) {
            return {};
        }

        unsigned char* b = (unsigned char*)binary.data();

        if (binary.length() == 4) {
            ipLength = snprintf(buf, 64, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        } else {
            ipLength =
                snprintf(buf, 64, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x", b[0], b[1],
                         b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }

        return {buf, (unsigned int)ipLength};
    }

    std::string_view getRemoteAddress() {
#ifdef UWS_REMOTE_ADDRESS_USERSPACE
        AsyncSocketData<SSL>* data = getAsyncSocketData();
        return std::string_view(data->remoteAddress, (unsigned int)data->remoteAddressLength);
#else
        static thread_local char buf[16];
        int ipLength = 16;
        us_socket_remote_address(SSL, (us_socket_t*)this, buf, &ipLength);
        return std::string_view(buf, (unsigned int)ipLength);
#endif
    }

    std::string_view getRemoteAddressAsText() {
        return addressAsText(getRemoteAddress());
    }

    unsigned int getRemotePort() {
        int port = us_socket_remote_port(SSL, (us_socket_t*)this);
        return (unsigned int)port;
    }

    std::pair<int, bool> write(const char* src, int length, bool optionally = false, int nextLength = 0) {
        if (us_socket_is_closed(SSL, (us_socket_t*)this)) {
            return {length, false};
        }

        LoopData* loopData = getLoopData();
        AsyncSocketData<SSL>* asyncSocketData = getAsyncSocketData();

        if (asyncSocketData->buffer.length()) {
            int written = us_socket_write(SSL, (us_socket_t*)this, asyncSocketData->buffer.data(),
                                          (int)asyncSocketData->buffer.length(), length);

            if ((unsigned int)written < asyncSocketData->buffer.length()) {
                asyncSocketData->buffer.erase((unsigned int)written);

                if (optionally) {
                    return {0, true};
                } else {
                    asyncSocketData->buffer.append(src, (unsigned int)length);

                    return {length, true};
                }
            }

            asyncSocketData->buffer.clear();
        }

        if (length) {
            if (loopData->corkedSocket == this) {
                if (LoopData::CORK_BUFFER_SIZE - loopData->corkOffset >= (unsigned int)length) {
                    memcpy(loopData->corkBuffer + loopData->corkOffset, src, (unsigned int)length);
                    loopData->corkOffset += (unsigned int)length;
                } else {
                    if constexpr (false) {
                        unsigned int stripped = LoopData::CORK_BUFFER_SIZE - loopData->corkOffset;
                        memcpy(loopData->corkBuffer + loopData->corkOffset, src, stripped);
                        loopData->corkOffset = LoopData::CORK_BUFFER_SIZE;

                        auto [written, failed] = uncork(src + stripped, length - (int)stripped, optionally);
                        return {written + (int)stripped, failed};
                    }

                    return uncork(src, length, optionally);
                }
            } else {
                int written = us_socket_write(SSL, (us_socket_t*)this, src, length, nextLength != 0);

                if (written < length) {
                    if (optionally) {
                        return {written, true};
                    }

                    if (nextLength) {
                        asyncSocketData->buffer.reserve(asyncSocketData->buffer.length() +
                                                        (size_t)(length - written + nextLength));
                    }

                    asyncSocketData->buffer.append(src + written, (size_t)(length - written));

                    return {length, true};
                }
            }
        }

        return {length, false};
    }

    std::pair<int, bool> uncork(const char* src = nullptr, int length = 0, bool optionally = false) {
        LoopData* loopData = getLoopData();

        if (loopData->corkedSocket == this) {
            loopData->corkedSocket = nullptr;

            if (loopData->corkOffset) {
                auto [written, failed] = write(loopData->corkBuffer, (int)loopData->corkOffset, false, length);
                loopData->corkOffset = 0;

                if (failed) {
                    if (!optionally && src && length) {
                        AsyncSocketData<SSL>* asyncSocketData = getAsyncSocketData();
                        asyncSocketData->buffer.append(src, (size_t)length);

                        return {length, true};
                    }

                    return {0, true};
                }
            }

            return write(src, length, optionally, 0);
        } else {
            return {0, false};
        }
    }
};

}  

#endif  // UWS_ASYNCSOCKET_H
