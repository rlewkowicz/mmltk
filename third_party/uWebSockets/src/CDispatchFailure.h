#ifndef UWS_CDISPATCHFAILURE_H
#define UWS_CDISPATCHFAILURE_H

namespace uWS {

// C callbacks cannot carry exceptions across libuSockets frames. Applications may install one
// allocation-free failure latch before registering routes; every HTTP and WebSocket child context
// copies this handle and invokes it after restoring its local dispatch invariants.
struct CDispatchFailureHandler {
    using Callback = void (*)(void* context) noexcept;

    void* context = nullptr;
    Callback callback = nullptr;

    void notify() const noexcept {
        if (callback != nullptr) {
            callback(context);
        }
    }
};

}  // namespace uWS

#endif  // UWS_CDISPATCHFAILURE_H
