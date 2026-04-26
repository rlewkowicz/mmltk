/*
 * Authored by Alex Hultman, 2018-2020.
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

/* todo: this looks identical to WebSocketBehavior, why not just std::move that entire thing in? */

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

}  // namespace uWS

#endif  // UWS_WEBSOCKETCONTEXTDATA_H
