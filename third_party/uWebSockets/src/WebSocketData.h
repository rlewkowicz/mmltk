
#ifndef UWS_WEBSOCKETDATA_H
#define UWS_WEBSOCKETDATA_H

#include "WebSocketProtocol.h"
#include "AsyncSocketData.h"
#include "PerMessageDeflate.h"
#include "TopicTree.h"

#include <string>

namespace uWS {

struct WebSocketData : AsyncSocketData<false>, WebSocketState<true> {
    template <bool, bool, typename>
    friend struct WebSocketContext;
    template <bool, typename>
    friend struct WebSocketContextData;
    template <bool, bool, typename>
    friend struct WebSocket;
    template <bool>
    friend struct HttpContext;

   private:
    std::string fragmentBuffer;
    unsigned int controlTipLength = 0;
    bool isShuttingDown = 0;
    bool hasTimedOut = false;
    enum CompressionStatus : char {
        DISABLED,
        ENABLED,
        COMPRESSED_FRAME
    } compressionStatus;

    DeflationStream* deflationStream = nullptr;
    InflationStream* inflationStream = nullptr;

    Subscriber* subscriber = nullptr;

   public:
    WebSocketData(bool perMessageDeflate, CompressOptions compressOptions, BackPressure&& backpressure)
        : AsyncSocketData<false>(std::move(backpressure)), WebSocketState<true>() {
        compressionStatus = perMessageDeflate ? ENABLED : DISABLED;

        if (perMessageDeflate) {
            if ((compressOptions & CompressOptions::_COMPRESSOR_MASK) != CompressOptions::SHARED_COMPRESSOR) {
                deflationStream = new DeflationStream(compressOptions);
            }
            if ((compressOptions & CompressOptions::_DECOMPRESSOR_MASK) != CompressOptions::SHARED_DECOMPRESSOR) {
                inflationStream = new InflationStream(compressOptions);
            }
        }
    }

    ~WebSocketData() {
        if (deflationStream) {
            delete deflationStream;
        }

        if (inflationStream) {
            delete inflationStream;
        }

        if (subscriber) {
            delete subscriber;
        }
    }
};

}  

#endif  // UWS_WEBSOCKETDATA_H
