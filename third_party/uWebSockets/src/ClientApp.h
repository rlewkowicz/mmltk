#include "MoveOnlyFunction.h"
#include "WebSocketContext.h"
#include <string>

namespace uWS {

struct WebSocketClientBehavior {
    MoveOnlyFunction<void()> open;
    MoveOnlyFunction<void()> message;
    MoveOnlyFunction<void()> close;
};

struct ClientApp {
    WebSocketContext<0, false, int>* webSocketContext;

    ClientApp(WebSocketClientBehavior&& behavior) {}

    ClientApp&& connect(std::string url, std::string protocol = "") {
        return std::move(*this);
    }

    void run() {}
};

}  // namespace uWS