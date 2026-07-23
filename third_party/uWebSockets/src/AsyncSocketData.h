
#ifndef UWS_ASYNCSOCKETDATA_H
#define UWS_ASYNCSOCKETDATA_H

#include <string>

namespace uWS {

struct BackPressure {
    std::string buffer;
    unsigned int pendingRemoval = 0;
    BackPressure(BackPressure&& other) {
        buffer = std::move(other.buffer);
        pendingRemoval = other.pendingRemoval;
    }
    BackPressure() = default;
    void append(const char* data, size_t length) {
        buffer.append(data, length);
    }
    void erase(unsigned int length) {
        pendingRemoval += length;
        if (pendingRemoval > (buffer.length() >> 5)) {
            std::string(buffer.begin() + pendingRemoval, buffer.end()).swap(buffer);
            pendingRemoval = 0;
        }
    }
    size_t length() {
        return buffer.length() - pendingRemoval;
    }
    void clear() {
        pendingRemoval = 0;
        buffer.clear();
        buffer.shrink_to_fit();
    }
    void reserve(size_t length) {
        buffer.reserve(length + pendingRemoval);
    }
    void resize(size_t length) {
        buffer.resize(length + pendingRemoval);
    }
    const char* data() {
        return buffer.data() + pendingRemoval;
    }
    size_t totalLength() {
        return buffer.length();
    }
};

template <bool SSL>
struct AsyncSocketData {
    BackPressure buffer;

#ifdef UWS_REMOTE_ADDRESS_USERSPACE
    char remoteAddress[16];
    int remoteAddressLength = 0;
#endif

    AsyncSocketData(BackPressure&& backpressure) : buffer(std::move(backpressure)) {}

    AsyncSocketData() = default;
};

}  

#endif  // UWS_ASYNCSOCKETDATA_H
