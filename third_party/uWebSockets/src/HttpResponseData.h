
#ifndef UWS_HTTPRESPONSEDATA_H
#define UWS_HTTPRESPONSEDATA_H

#include "HttpParser.h"
#include "AsyncSocketData.h"
#include "ProxyParser.h"

#include "MoveOnlyFunction.h"

namespace uWS {

template <bool SSL>
struct HttpResponseData : AsyncSocketData<SSL>, HttpParser {
    template <bool>
    friend struct HttpResponse;
    template <bool>
    friend struct HttpContext;

    void markDone() {
        onAborted = nullptr;
        onWritable = nullptr;

        state &= ~HttpResponseData<SSL>::HTTP_RESPONSE_PENDING;
    }

    bool callOnWritable(uintmax_t offset) {
        MoveOnlyFunction<bool(uintmax_t)> borrowedOnWritable = std::move(onWritable);

        onWritable = [](uintmax_t) { return true; };

        bool ret = borrowedOnWritable(offset);

        if (onWritable) {
            onWritable = std::move(borrowedOnWritable);
        }

        return ret;
    }

   private:
    enum {
        HTTP_STATUS_CALLED = 1,
        HTTP_WRITE_CALLED = 2,
        HTTP_END_CALLED = 4,
        HTTP_RESPONSE_PENDING = 8,
        HTTP_CONNECTION_CLOSE = 16
    };

    MoveOnlyFunction<bool(uintmax_t)> onWritable;
    MoveOnlyFunction<void()> onAborted;
    MoveOnlyFunction<void(std::string_view, uint64_t)> inStream;
    uintmax_t offset = 0;

    unsigned int received_bytes_per_timeout = 0;

    int state = 0;

#ifdef UWS_WITH_PROXY
    ProxyParser proxyParser;
#endif
};

}  

#endif  // UWS_HTTPRESPONSEDATA_H
