
extern "C" {
#include "quic.h"
}

#include "Http3ContextData.h"
#include "Http3ResponseData.h"

namespace uWS {
struct Http3Context {
    static Http3Context* create(us_loop_t* loop, us_quic_socket_context_options_t options) {
        auto* context = us_create_quic_socket_context(loop, options, sizeof(Http3ContextData));

        us_quic_socket_context_on_stream_data(context, [](us_quic_stream_t* s, char* data, int length) {
            Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext(s);

            if (responseData->onData) {
                responseData->onData({data, (size_t)length}, false);
            }
        });
        us_quic_socket_context_on_stream_end(context, [](us_quic_stream_t* s) {
            Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext(s);

            if (responseData->onData) {
                responseData->onData({nullptr, 0}, true);
            }
        });
        us_quic_socket_context_on_stream_open(context, [](us_quic_stream_t* s, int is_client) {
            printf("Stream open!\n");

            new (us_quic_stream_ext(s)) Http3ResponseData();
        });
        us_quic_socket_context_on_close(context, [](us_quic_socket_t* s) { printf("QUIC socket disconnected!\n"); });
        us_quic_socket_context_on_stream_writable(context, [](us_quic_stream_t* s) {
            Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext(s);

            if (responseData->onWritable) {
                responseData->onWritable(responseData->offset);
            } else {
                int written = us_quic_stream_write(s, (char*)responseData->backpressure.data(),
                                                   responseData->backpressure.length());
                responseData->backpressure.erase(written);

                if (responseData->backpressure.length() == 0) {
                    printf("wrote until end, shutting down now!\n");
                    us_quic_stream_shutdown(s);
                    us_quic_stream_close(s);
                }
            }
        });
        us_quic_socket_context_on_stream_headers(context, [](us_quic_stream_t* s) {
            Http3ContextData* contextData =
                (Http3ContextData*)us_quic_socket_context_ext(us_quic_socket_context(us_quic_stream_socket(s)));

            Http3Request* req = nullptr;

            std::string_view upperCasedMethod = req->getHeader(":method");
            std::string_view path = req->getHeader(":path");

            contextData->router.getUserData() = {(Http3Response*)s, (Http3Request*)nullptr};
            contextData->router.route(upperCasedMethod, path);
        });
        us_quic_socket_context_on_open(context,
                                       [](us_quic_socket_t* s, int is_client) { printf("QUIC socket connected!\n"); });
        us_quic_socket_context_on_stream_close(context, [](us_quic_stream_t* s) {
            printf("Stream closed!\n");

            Http3ResponseData* responseData = (Http3ResponseData*)us_quic_stream_ext(s);

            if (responseData->onAborted) {
                responseData->onAborted();
            }

            responseData->~Http3ResponseData();
        });

        return (Http3Context*)context;
    }

    us_quic_listen_socket_t* listen(const char* host, int port) {
        us_quic_listen_socket_t* listen_socket =
            us_quic_socket_context_listen((us_quic_socket_context_t*)this, host, port, sizeof(Http3ResponseData));

        return listen_socket;
    }

    void init() {
        Http3ContextData* contextData = (Http3ContextData*)us_quic_socket_context_ext((us_quic_socket_context_t*)this);

        new (contextData) Http3ContextData();
    }

    void onHttp(std::string method, std::string path, MoveOnlyFunction<void(Http3Response*, Http3Request*)>&& cb) {
        Http3ContextData* contextData = (Http3ContextData*)us_quic_socket_context_ext((us_quic_socket_context_t*)this);

        std::vector<std::string> methods;
        if (method == "*") {
            methods = {"*"};
        } else {
            methods = {method};
        }

        contextData->router.add(methods, path,
                                [handler = std::move(cb)](HttpRouter<Http3ContextData::RouterData>* router) mutable {
                                    Http3ContextData::RouterData& routerData = router->getUserData();

                                    handler(routerData.res, routerData.req);

                                    return true;
                                });
    }
};
}  // namespace uWS