
#ifndef UWS_WEBSOCKETCONTEXTDATA_H
#define UWS_WEBSOCKETCONTEXTDATA_H

#include <string_view>
#include <vector>

#include "AsyncSocket.h"
#include "CDispatchFailure.h"
#include "Loop.h"
#include "MoveOnlyFunction.h"
#include "TopicTree.h"
#include "WebSocketData.h"
#include "WebSocketHandlers.h"
#include "WebSocketProtocol.h"

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

/* Generated from the shared handler list so these slots cannot drift from WebSocketBehavior's. */
#define UWS_CONTEXT_HANDLER_SLOT(name, ...) MoveOnlyFunction<__VA_ARGS__> name##Handler = nullptr;
    UWS_WEBSOCKET_HANDLERS(UWS_CONTEXT_HANDLER_SLOT, SSL, USERDATA)
#undef UWS_CONTEXT_HANDLER_SLOT

    size_t maxPayloadLength = 0;

    CompressOptions compression;
    CDispatchFailureHandler cDispatchFailureHandler;

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
