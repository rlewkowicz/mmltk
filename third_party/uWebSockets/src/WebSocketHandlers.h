#ifndef UWS_WEBSOCKETHANDLERS_H
#define UWS_WEBSOCKETHANDLERS_H

/* The websocket callback set used to be written out three times: as the slots a caller fills in
 * (App.h's WebSocketBehavior), as the slots the socket context invokes (WebSocketContextData), and
 * as the moves that transfer the former into the latter (TemplatedApp::ws). The list lives here
 * once so those three cannot drift apart.
 *
 * SSL_ and USERDATA_ are parameters because the two structs spell their template parameters
 * differently. X is expanded as X(name, signature...) and must be variadic: the commas inside the
 * WebSocket<> template argument list are not protected by parentheses, so the signature arrives
 * split across the trailing arguments and __VA_ARGS__ puts it back together. WebSocketBehavior's
 * `upgrade` slot is deliberately not part of the list; it is an HTTP-side callback consumed during
 * the upgrade handshake and has no counterpart in the context data.
 *
 * WebSocketBehavior is initialised with designated initializers, so this order is also the order
 * callers must write: adding a handler means appending to this list. */
#define UWS_WEBSOCKET_HANDLERS(X, SSL_, USERDATA_)                                       \
    X(open, void(WebSocket<SSL_, true, USERDATA_>*))                                     \
    X(message, void(WebSocket<SSL_, true, USERDATA_>*, std::string_view, OpCode))        \
    X(dropped, void(WebSocket<SSL_, true, USERDATA_>*, std::string_view, OpCode))        \
    X(drain, void(WebSocket<SSL_, true, USERDATA_>*))                                    \
    X(ping, void(WebSocket<SSL_, true, USERDATA_>*, std::string_view))                   \
    X(pong, void(WebSocket<SSL_, true, USERDATA_>*, std::string_view))                   \
    X(subscription, void(WebSocket<SSL_, true, USERDATA_>*, std::string_view, int, int)) \
    X(close, void(WebSocket<SSL_, true, USERDATA_>*, int, std::string_view))

#endif  // UWS_WEBSOCKETHANDLERS_H
