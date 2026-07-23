
#ifndef UWS_WEBSOCKETCONTEXTDATA_H
#define UWS_WEBSOCKETCONTEXTDATA_H

#include "Loop.h"
#include "AsyncSocket.h"

#include "MoveOnlyFunction.h"
#include <string_view>
#include <vector>

#include "WebSocketProtocol.h"
#include "TopicTree.h"
#include "WebSocketData.h"

namespace uWS {

struct TopicTreeMessage {
    std::string message;
    int opCode;
    bool compress;
};
struct TopicTreeBigMessage {
    std::string_view message;
    int opCode;
    bool compress;
};

template <bool, bool, typename>
struct WebSocket;


template <bool SSL, typename USERDATA>
struct WebSocketContextData {
   private:
   public:
    TopicTree<TopicTreeMessage, TopicTreeBigMessage>* topicTree;

    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*)> openHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*, std::string_view, OpCode)> messageHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*, std::string_view, OpCode)> droppedHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*)> drainHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*, std::string_view, int, int)> subscriptionHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*, int, std::string_view)> closeHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*, std::string_view)> pingHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, true, USERDATA>*, std::string_view)> pongHandler = nullptr;

    size_t maxPayloadLength = 0;

    CompressOptions compression;

    size_t maxBackpressure = 0;
    bool closeOnBackpressureLimit;
    bool resetIdleTimeoutOnSend;
    bool sendPingsAutomatically;
    unsigned short maxLifetime;

    std::pair<unsigned short, unsigned short> idleTimeoutComponents;

    void calculateIdleTimeoutCompnents(unsigned short idleTimeout) {
        unsigned short margin = 4;
        while ((int)idleTimeout - margin * 2 >= margin * 2 && margin < 16) {
            margin = (unsigned short)(margin << 1);
        }
        idleTimeoutComponents = {idleTimeout - (sendPingsAutomatically ? margin : 0), margin};
    }

    ~WebSocketContextData() {}

    WebSocketContextData(TopicTree<TopicTreeMessage, TopicTreeBigMessage>* topicTree) : topicTree(topicTree) {}
};

}  

#endif  // UWS_WEBSOCKETCONTEXTDATA_H
