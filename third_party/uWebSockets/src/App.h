
#ifndef UWS_APP_H
#define UWS_APP_H

#define _CRT_SECURE_NO_WARNINGS

#include <string>
#include <charconv>
#include <string_view>

namespace uWS {
inline bool hasBrokenCompression(std::string_view userAgent) {
    size_t posStart = userAgent.find(" Version/15.");
    if (posStart == std::string_view::npos)
        return false;
    posStart += 12;

    size_t posEnd = userAgent.find(' ', posStart);
    if (posEnd == std::string_view::npos)
        return false;

    unsigned int minorVersion = 0;
    auto result = std::from_chars(userAgent.data() + posStart, userAgent.data() + posEnd, minorVersion);
    if (result.ec != std::errc())
        return false;
    if (result.ptr != userAgent.data() + posEnd)
        return false;
    if (minorVersion > 3)
        return false;

    if (userAgent.find(" Safari/", posEnd) == std::string_view::npos)
        return false;

    return true;
}
}  

#include "HttpContext.h"
#include "HttpResponse.h"
#include "WebSocketContext.h"
#include "WebSocket.h"
#include "PerMessageDeflate.h"

namespace uWS {

struct SocketContextOptions {
    const char* key_file_name = nullptr;
    const char* cert_file_name = nullptr;
    const char* passphrase = nullptr;
    const char* dh_params_file_name = nullptr;
    const char* ca_file_name = nullptr;
    const char* ssl_ciphers = nullptr;
    int ssl_prefer_low_memory_usage = 0;

    operator struct us_socket_context_options_t() const {
        struct us_socket_context_options_t socket_context_options;
        memcpy(&socket_context_options, this, sizeof(SocketContextOptions));
        return socket_context_options;
    }
};

static_assert(sizeof(struct us_socket_context_options_t) == sizeof(SocketContextOptions),
              "Mismatching uSockets/uWebSockets ABI");

template <bool SSL>
struct TemplatedApp {
   private:
    HttpContext<SSL>* httpContext;
    std::vector<MoveOnlyFunction<void()>> webSocketContextDeleters;

    std::vector<void*> webSocketContexts;

   public:
    TopicTree<TopicTreeMessage, TopicTreeBigMessage>* topicTree = nullptr;

    TemplatedApp&& addServerName(std::string hostname_pattern, SocketContextOptions options = {}) {
        if constexpr (SSL) {
            auto* domainRouter = new HttpRouter<typename HttpContextData<SSL>::RouterData>();

            us_socket_context_add_server_name(SSL, (struct us_socket_context_t*)httpContext, hostname_pattern.c_str(),
                                              options, domainRouter);
        }

        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& removeServerName(std::string hostname_pattern) {
        auto* domainRouter = us_socket_context_find_server_name_userdata(SSL, (struct us_socket_context_t*)httpContext,
                                                                         hostname_pattern.c_str());
        if (domainRouter) {
            delete (HttpRouter<typename HttpContextData<SSL>::RouterData>*)domainRouter;
        }

        us_socket_context_remove_server_name(SSL, (struct us_socket_context_t*)httpContext, hostname_pattern.c_str());
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& missingServerName(MoveOnlyFunction<void(const char* hostname)> handler) {
        if (!constructorFailed()) {
            httpContext->getSocketContextData()->missingServerNameHandler = std::move(handler);

            us_socket_context_on_server_name(
                SSL, (struct us_socket_context_t*)httpContext,
                [](struct us_socket_context_t* context, const char* hostname) {
                    HttpContext<SSL>* httpContext = (HttpContext<SSL>*)context;
                    httpContext->getSocketContextData()->missingServerNameHandler(hostname);
                });
        }

        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    void* getNativeHandle() {
        return us_socket_context_get_native_handle(SSL, (struct us_socket_context_t*)httpContext);
    }

    TemplatedApp&& filter(MoveOnlyFunction<void(HttpResponse<SSL>*, int)>&& filterHandler) {
        httpContext->filter(std::move(filterHandler));

        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    bool publishPrepared(std::string_view topic, PreparedMessage& preparedMessage) {
        return reinterpret_cast<TopicTree<TopicTreeMessage, PreparedMessage*>*>(topicTree)->publishBig(
            nullptr, topic, &preparedMessage, [](Subscriber* s, PreparedMessage* preparedMessage) {
                auto* ws = (WebSocket<SSL, true, int>*)s->user;

                ws->sendPrepared(*preparedMessage);
            });
    }

    bool publish(std::string_view topic, std::string_view message, OpCode opCode, bool compress = false) {
        if (message.length() >= LoopData::CORK_BUFFER_SIZE) {
            return topicTree->publishBig(nullptr, topic, {message, opCode, compress},
                                         [](Subscriber* s, TopicTreeBigMessage& message) {
                                             auto* ws = (WebSocket<SSL, true, int>*)s->user;

                                             ws->send(message.message, (OpCode)message.opCode, message.compress);
                                         });
        } else {
            return topicTree->publish(nullptr, topic, {std::string(message), opCode, compress});
        }
    }

    unsigned int numSubscribers(std::string_view topic) {
        Topic* t = topicTree->lookupTopic(topic);
        if (t) {
            return (unsigned int)t->size();
        }

        return 0;
    }

    ~TemplatedApp() {
        if (httpContext) {
            httpContext->free();

            for (auto& webSocketContextDeleter : webSocketContextDeleters) {
                webSocketContextDeleter();
            }
        }

        if (topicTree) {
            Loop::get()->removePostHandler(topicTree);
            Loop::get()->removePreHandler(topicTree);

            delete topicTree;
        }
    }

    TemplatedApp(const TemplatedApp& other) = delete;

    TemplatedApp(TemplatedApp&& other) {
        httpContext = other.httpContext;
        other.httpContext = nullptr;

        webSocketContextDeleters = std::move(other.webSocketContextDeleters);

        webSocketContexts = std::move(other.webSocketContexts);

        topicTree = other.topicTree;
        other.topicTree = nullptr;
    }

    TemplatedApp(SocketContextOptions options = {}) {
        httpContext = HttpContext<SSL>::create(Loop::get(), options);

        this->any("/*", [](auto* res, auto*) {
            res->writeStatus("404 File Not Found");
            res->end("<html><body><h1>File Not Found</h1><hr><i>uWebSockets/20 Server</i></body></html>");
        });
    }

    TemplatedApp& operator=(const TemplatedApp&) = delete;

    TemplatedApp& operator=(TemplatedApp&& other) {
        std::swap(this->httpContext, other.httpContext);
        std::swap(this->topicTree, other.topicTree);
        std::swap(this->webSocketContextDeleters, other.webSocketContextDeleters);
        std::swap(this->webSocketContexts, other.webSocketContexts);
    }

    bool constructorFailed() {
        return !httpContext;
    }

    template <typename UserData>
    struct WebSocketBehavior {
        CompressOptions compression = DISABLED;
        unsigned int maxPayloadLength = 16 * 1024;
        unsigned short idleTimeout = 120;
        unsigned int maxBackpressure = 64 * 1024;
        bool closeOnBackpressureLimit = false;
        bool resetIdleTimeoutOnSend = false;
        bool sendPingsAutomatically = true;
        unsigned short maxLifetime = 0;
        MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*, struct us_socket_context_t*)> upgrade = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*)> open = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*, std::string_view, OpCode)> message = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*, std::string_view, OpCode)> dropped = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*)> drain = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*, std::string_view)> ping = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*, std::string_view)> pong = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*, std::string_view, int, int)> subscription = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, true, UserData>*, int, std::string_view)> close = nullptr;
    };

    TemplatedApp&& close() {
        us_socket_context_close(SSL, (struct us_socket_context_t*)httpContext);
        for (void* webSocketContext : webSocketContexts) {
            us_socket_context_close(SSL, (struct us_socket_context_t*)webSocketContext);
        }

        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    template <typename UserData>
    TemplatedApp&& ws(std::string pattern, WebSocketBehavior<UserData>&& behavior) {
        static_assert(alignof(UserData) <= LIBUS_EXT_ALIGNMENT,
                      "µWebSockets cannot satisfy UserData alignment requirements. You need to recompile µSockets with "
                      "LIBUS_EXT_ALIGNMENT adjusted accordingly.");

        if (!httpContext) {
            return std::move(static_cast<TemplatedApp&&>(*this));
        }

        if (behavior.idleTimeout && behavior.idleTimeout < 8) {
            std::cerr << "Error: idleTimeout must be either 0 or greater than 8!" << std::endl;
            std::terminate();
        }

        if (behavior.idleTimeout > 240 * 4) {
            std::cerr << "Error: idleTimeout must not be greater than 960 seconds!" << std::endl;
            std::terminate();
        }

        if (behavior.maxLifetime > 240) {
            std::cerr << "Error: maxLifetime must not be greater than 240 minutes!" << std::endl;
            std::terminate();
        }

        if (!topicTree) {
            bool needsUncork = false;
            topicTree = new TopicTree<TopicTreeMessage, TopicTreeBigMessage>(
                [needsUncork](Subscriber* s, TopicTreeMessage& message,
                              TopicTree<TopicTreeMessage, TopicTreeBigMessage>::IteratorFlags flags) mutable {
                    auto* ws = (WebSocket<SSL, true, int>*)s->user;

                    if (flags & TopicTree<TopicTreeMessage, TopicTreeBigMessage>::IteratorFlags::FIRST) {
                        if (ws->canCork() && !ws->isCorked()) {
                            ((AsyncSocket<SSL>*)ws)->cork();
                            needsUncork = true;
                        }
                    }

                    if (WebSocket<SSL, true, int>::SendStatus::DROPPED ==
                        ws->send(message.message, (OpCode)message.opCode, message.compress)) {
                        if (needsUncork) {
                            ((AsyncSocket<SSL>*)ws)->uncork();
                            needsUncork = false;
                        }
                        return true;
                    }

                    if (flags & TopicTree<TopicTreeMessage, TopicTreeBigMessage>::IteratorFlags::LAST) {
                        if (needsUncork) {
                            ((AsyncSocket<SSL>*)ws)->uncork();
                        }
                    }

                    return false;
                });

            Loop::get()->addPostHandler(topicTree, [topicTree = topicTree](Loop*) { topicTree->drain(); });

            Loop::get()->addPreHandler(topicTree, [topicTree = topicTree](Loop*) { topicTree->drain(); });
        }

        auto* webSocketContext =
            WebSocketContext<SSL, true, UserData>::create(Loop::get(), (us_socket_context_t*)httpContext, topicTree);

        webSocketContextDeleters.push_back([webSocketContext]() { webSocketContext->free(); });

        webSocketContexts.push_back((void*)webSocketContext);

#ifdef UWS_NO_ZLIB
        behavior.compression = DISABLED;
#endif

        if (behavior.compression) {
            LoopData* loopData =
                (LoopData*)us_loop_ext(us_socket_context_loop(SSL, webSocketContext->getSocketContext()));

            if (!loopData->zlibContext) {
                loopData->zlibContext = new ZlibContext;
                loopData->inflationStream = new InflationStream(CompressOptions::DEDICATED_DECOMPRESSOR);
                loopData->deflationStream = new DeflationStream(CompressOptions::DEDICATED_COMPRESSOR);
            }
        }

        webSocketContext->getExt()->openHandler = std::move(behavior.open);
        webSocketContext->getExt()->messageHandler = std::move(behavior.message);
        webSocketContext->getExt()->droppedHandler = std::move(behavior.dropped);
        webSocketContext->getExt()->drainHandler = std::move(behavior.drain);
        webSocketContext->getExt()->subscriptionHandler = std::move(behavior.subscription);
        webSocketContext->getExt()->closeHandler = std::move(behavior.close);
        webSocketContext->getExt()->pingHandler = std::move(behavior.ping);
        webSocketContext->getExt()->pongHandler = std::move(behavior.pong);

        webSocketContext->getExt()->maxPayloadLength = behavior.maxPayloadLength;
        webSocketContext->getExt()->maxBackpressure = behavior.maxBackpressure;
        webSocketContext->getExt()->closeOnBackpressureLimit = behavior.closeOnBackpressureLimit;
        webSocketContext->getExt()->resetIdleTimeoutOnSend = behavior.resetIdleTimeoutOnSend;
        webSocketContext->getExt()->sendPingsAutomatically = behavior.sendPingsAutomatically;
        webSocketContext->getExt()->maxLifetime = behavior.maxLifetime;
        webSocketContext->getExt()->compression = behavior.compression;

        webSocketContext->getExt()->calculateIdleTimeoutCompnents(behavior.idleTimeout);

        httpContext->onHttp(
            "GET", pattern,
            [webSocketContext, behavior = std::move(behavior)](auto* res, auto* req) mutable {
                std::string_view secWebSocketKey = req->getHeader("sec-websocket-key");
                if (secWebSocketKey.length() == 24) {
                    if (behavior.upgrade) {
                        if (hasBrokenCompression(req->getHeader("user-agent"))) {
                            std::string_view secWebSocketExtensions = req->getHeader("sec-websocket-extensions");
                            memset((void*)secWebSocketExtensions.data(), ' ', secWebSocketExtensions.length());
                        }

                        behavior.upgrade(res, req, (struct us_socket_context_t*)webSocketContext);
                    } else {
                        std::string_view secWebSocketProtocol = req->getHeader("sec-websocket-protocol");
                        std::string_view secWebSocketExtensions = req->getHeader("sec-websocket-extensions");

                        if (hasBrokenCompression(req->getHeader("user-agent"))) {
                            secWebSocketExtensions = "";
                        }

                        res->template upgrade<UserData>({}, secWebSocketKey, secWebSocketProtocol,
                                                        secWebSocketExtensions,
                                                        (struct us_socket_context_t*)webSocketContext);
                    }

                } else {
                    req->setYield(true);
                }
            },
            true);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& domain(std::string serverName) {
        HttpContextData<SSL>* httpContextData = httpContext->getSocketContextData();

        void* domainRouter = us_socket_context_find_server_name_userdata(SSL, (struct us_socket_context_t*)httpContext,
                                                                         serverName.c_str());
        if (domainRouter) {
            std::cout << "Browsed to SNI: " << serverName << std::endl;
            httpContextData->currentRouter = (decltype(httpContextData->currentRouter))domainRouter;
        } else {
            std::cout << "Cannot browse to SNI: " << serverName << std::endl;
            httpContextData->currentRouter = &httpContextData->router;
        }

        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& get(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("GET", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& post(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("POST", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& options(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("OPTIONS", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& del(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("DELETE", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& patch(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("PATCH", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& put(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("PUT", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& head(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("HEAD", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& connect(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("CONNECT", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& trace(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("TRACE", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& any(std::string pattern, MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler) {
        if (httpContext) {
            httpContext->onHttp("*", pattern, std::move(handler));
        }
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& listen(std::string host, int port, MoveOnlyFunction<void(us_listen_socket_t*)>&& handler) {
        if (!host.length()) {
            return listen(port, std::move(handler));
        }
        handler(httpContext ? httpContext->listen(host.c_str(), port, 0) : nullptr);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& listen(std::string host, int port, int options,
                          MoveOnlyFunction<void(us_listen_socket_t*)>&& handler) {
        if (!host.length()) {
            return listen(port, options, std::move(handler));
        }
        handler(httpContext ? httpContext->listen(host.c_str(), port, options) : nullptr);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& listen(int port, MoveOnlyFunction<void(us_listen_socket_t*)>&& handler) {
        handler(httpContext ? httpContext->listen(nullptr, port, 0) : nullptr);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& listen(int port, int options, MoveOnlyFunction<void(us_listen_socket_t*)>&& handler) {
        handler(httpContext ? httpContext->listen(nullptr, port, options) : nullptr);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& listen(int options, MoveOnlyFunction<void(us_listen_socket_t*)>&& handler, std::string path) {
        handler(httpContext ? httpContext->listen(path.c_str(), options) : nullptr);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& listen(MoveOnlyFunction<void(us_listen_socket_t*)>&& handler, std::string path) {
        handler(httpContext ? httpContext->listen(path.c_str(), 0) : nullptr);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& preOpen(LIBUS_SOCKET_DESCRIPTOR (*handler)(struct us_socket_context_t*, LIBUS_SOCKET_DESCRIPTOR,
                                                              char*, int)) {
        httpContext->onPreOpen(handler);
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& removeChildApp(TemplatedApp* app) {
        auto& childApps = httpContext->getSocketContextData()->childApps;
        childApps.erase(std::remove(childApps.begin(), childApps.end(), (void*)app), childApps.end());
        httpContext->getSocketContextData()->roundRobin = 0;

        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& addChildApp(TemplatedApp* app) {
        httpContext->getSocketContextData()->childApps.push_back((void*)app);

        httpContext->onPreOpen([](struct us_socket_context_t* context, LIBUS_SOCKET_DESCRIPTOR fd, char* ip,
                                  int ip_length) -> LIBUS_SOCKET_DESCRIPTOR {
            HttpContext<SSL>* httpContext = (HttpContext<SSL>*)context;

            if (httpContext->getSocketContextData()->childApps.empty()) {
                return fd;
            }

            unsigned int* roundRobin = &httpContext->getSocketContextData()->roundRobin;

            TemplatedApp* receivingApp = (TemplatedApp*)httpContext->getSocketContextData()->childApps[*roundRobin];

            receivingApp->getLoop()->defer([fd, ipStore = std::string(ip, ip + ip_length), receivingApp]() {
                receivingApp->adoptSocket(fd, std::string_view(ipStore));
            });

            if (++(*roundRobin) == httpContext->getSocketContextData()->childApps.size()) {
                *roundRobin = 0;
            }

            return fd + 1;
        });
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& adoptSocket(LIBUS_SOCKET_DESCRIPTOR accepted_fd, std::string_view ip = std::string_view()) {
        httpContext->adoptAcceptedSocket(accepted_fd, (char*)ip.data(), (int)ip.length());
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    TemplatedApp&& run() {
        uWS::run();
        return std::move(static_cast<TemplatedApp&&>(*this));
    }

    Loop* getLoop() {
        return (Loop*)httpContext->getLoop();
    }
};

}  

namespace uWS {
typedef uWS::TemplatedApp<false> App;
typedef uWS::TemplatedApp<true> SSLApp;
}  

#endif  // UWS_APP_H
