/*
 * Authored by Alex Hultman, 2018-2026.
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

#ifndef UWS_HTTPCONTEXT_H
#define UWS_HTTPCONTEXT_H

#include "Loop.h"
#include "HttpContextData.h"
#include "HttpResponseData.h"
#include "AsyncSocket.h"
#include "WebSocketData.h"

#include <string_view>
#include <iostream>
#include "MoveOnlyFunction.h"

namespace uWS {
template <bool>
struct HttpResponse;

template <bool SSL>
struct HttpContext {
    template <bool>
    friend struct TemplatedApp;
    template <bool>
    friend struct HttpResponse;

   private:
    HttpContext() = delete;

    static const int HTTP_IDLE_TIMEOUT_S = 10;

    static const int HTTP_RECEIVE_THROUGHPUT_BYTES = 16 * 1024;

    us_loop_t* getLoop() {
        return us_socket_context_loop(SSL, getSocketContext());
    }

    us_socket_context_t* getSocketContext() {
        return (us_socket_context_t*)this;
    }

    static us_socket_context_t* getSocketContext(us_socket_t* s) {
        return (us_socket_context_t*)us_socket_context(SSL, s);
    }

    HttpContextData<SSL>* getSocketContextData() {
        return (HttpContextData<SSL>*)us_socket_context_ext(SSL, getSocketContext());
    }

    static HttpContextData<SSL>* getSocketContextDataS(us_socket_t* s) {
        return (HttpContextData<SSL>*)us_socket_context_ext(SSL, getSocketContext(s));
    }

    HttpContext<SSL>* init() {
        us_socket_context_on_open(SSL, getSocketContext(), [](us_socket_t* s, int, char* ip, int ip_length) {
            us_socket_timeout(SSL, s, HTTP_IDLE_TIMEOUT_S);

            new (us_socket_ext(SSL, s)) HttpResponseData<SSL>;

#ifdef UWS_REMOTE_ADDRESS_USERSPACE
            AsyncSocketData<SSL>* asyncSocketData = (AsyncSocketData<SSL>*)us_socket_ext(SSL, s);
            if (ip_length > 0 && ip_length <= 16) {
                memcpy(asyncSocketData->remoteAddress, ip, (size_t)ip_length);
                asyncSocketData->remoteAddressLength = ip_length;
            } else {
                asyncSocketData->remoteAddressLength = 0;
            }
#else
            (void) ip;
            (void) ip_length;
#endif

            HttpContextData<SSL>* httpContextData = getSocketContextDataS(s);
            for (auto& f : httpContextData->filterHandlers) {
                f((HttpResponse<SSL>*)s, 1);
            }

            return s;
        });

        us_socket_context_on_close(SSL, getSocketContext(), [](us_socket_t* s, int, void*) {
            HttpResponseData<SSL>* httpResponseData = (HttpResponseData<SSL>*)us_socket_ext(SSL, s);

            HttpContextData<SSL>* httpContextData = getSocketContextDataS(s);
            for (auto& f : httpContextData->filterHandlers) {
                f((HttpResponse<SSL>*)s, -1);
            }

            if (httpResponseData->onAborted) {
                httpResponseData->onAborted();
            }

            httpResponseData->~HttpResponseData<SSL>();

            return s;
        });

        us_socket_context_on_data(SSL, getSocketContext(), [](us_socket_t* s, char* data, int length) {
            HttpContextData<SSL>* httpContextData = getSocketContextDataS(s);

            if (us_socket_is_shut_down(SSL, (us_socket_t*)s)) {
                return s;
            }

            HttpResponseData<SSL>* httpResponseData = (HttpResponseData<SSL>*)us_socket_ext(SSL, s);

            ((AsyncSocket<SSL>*)s)->cork();

            httpContextData->isParsingHttp = true;

            void* proxyParser = nullptr;
#ifdef UWS_WITH_PROXY
            proxyParser = &httpResponseData->proxyParser;
#endif

            auto [err, returnedSocket] = httpResponseData->consumePostPadded(
                data, (unsigned int)length, s, proxyParser,
                [httpContextData](void* s, HttpRequest* httpRequest) -> void* {
                    us_socket_timeout(SSL, (us_socket_t*)s, 0);

                    HttpResponseData<SSL>* httpResponseData =
                        (HttpResponseData<SSL>*)us_socket_ext(SSL, (us_socket_t*)s);
                    httpResponseData->offset = 0;

                    if (httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING) {
                        us_socket_close(SSL, (us_socket_t*)s, 0, nullptr);
                        return nullptr;
                    }

                    httpResponseData->state = HttpResponseData<SSL>::HTTP_RESPONSE_PENDING;

                    if (httpRequest->isAncient() || httpRequest->getHeader("connection").length() == 5) {
                        httpResponseData->state |= HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE;
                    }

                    auto* selectedRouter = &httpContextData->router;
                    if constexpr (SSL) {
                        void* domainRouter = us_socket_server_name_userdata(SSL, (struct us_socket_t*)s);
                        if (domainRouter) {
                            selectedRouter = (decltype(selectedRouter))domainRouter;
                        }
                    }

                    selectedRouter->getUserData() = {(HttpResponse<SSL>*)s, httpRequest};
                    if (!selectedRouter->route(httpRequest->getCaseSensitiveMethod(), httpRequest->getUrl())) {
                        us_socket_close(SSL, (us_socket_t*)s, 0, nullptr);
                        return nullptr;
                    }

                    if (httpContextData->upgradedWebSocket) {
                        return nullptr;
                    }

                    if (us_socket_is_closed(SSL, (struct us_socket_t*)s)) {
                        return nullptr;
                    }

                    if (us_socket_is_shut_down(SSL, (us_socket_t*)s)) {
                        return nullptr;
                    }

                    if (!((HttpResponse<SSL>*)s)->hasResponded() && !httpResponseData->onAborted) {
                        std::cerr << "Error: Returning from a request handler without responding or attaching an abort "
                                     "handler is forbidden!"
                                  << std::endl
                                  << "\tMethod: \"" << httpRequest->getCaseSensitiveMethod() << "\"" << std::endl
                                  << "\tURL: \"" << httpRequest->getUrl() << "\"" << std::endl;
                        std::terminate();
                    }

                    if (!((HttpResponse<SSL>*)s)->hasResponded() && httpResponseData->inStream) {
                        us_socket_timeout(SSL, (us_socket_t*)s, HTTP_IDLE_TIMEOUT_S);
                    }

                    return s;
                },
                [httpResponseData](void* user, std::string_view data, uint64_t maxRemainingBodyLength) -> void* {
                    if (httpResponseData->inStream) {
                        if (maxRemainingBodyLength == 0) {
                            us_socket_timeout(SSL, (struct us_socket_t*)user, 0);
                        } else {
                            httpResponseData->received_bytes_per_timeout += (unsigned int)data.length();
                            if (httpResponseData->received_bytes_per_timeout >=
                                HTTP_RECEIVE_THROUGHPUT_BYTES * HTTP_IDLE_TIMEOUT_S) {
                                us_socket_timeout(SSL, (struct us_socket_t*)user, HTTP_IDLE_TIMEOUT_S);
                                httpResponseData->received_bytes_per_timeout = 0;
                            }
                        }

                        httpResponseData->inStream(data, maxRemainingBodyLength);

                        if (us_socket_is_closed(SSL, (struct us_socket_t*)user)) {
                            return nullptr;
                        }

                        if (us_socket_is_shut_down(SSL, (us_socket_t*)user)) {
                            return nullptr;
                        }

                        if (maxRemainingBodyLength == 0) {
                            httpResponseData->inStream = nullptr;
                        }
                    }
                    return user;
                });

            httpContextData->isParsingHttp = false;

            if (returnedSocket == FULLPTR) {
                us_socket_write(SSL, s, httpErrorResponses[err].data(), (int)httpErrorResponses[err].length(), false);
                us_socket_shutdown(SSL, s);
                us_socket_close(SSL, s, 0, nullptr);
                returnedSocket = nullptr;
            }

            if (returnedSocket != nullptr) {
                auto [written, failed] = ((AsyncSocket<SSL>*)returnedSocket)->uncork();
                if (failed) {
                    ((AsyncSocket<SSL>*)s)->timeout(HTTP_IDLE_TIMEOUT_S);
                }

                if (httpResponseData->state & HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE) {
                    if ((httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING) == 0) {
                        if (((AsyncSocket<SSL>*)s)->getBufferedAmount() == 0) {
                            ((AsyncSocket<SSL>*)s)->shutdown();
                            ((AsyncSocket<SSL>*)s)->close();
                        }
                    }
                }

                return (us_socket_t*)returnedSocket;
            }

            if (httpContextData->upgradedWebSocket) {
                AsyncSocket<SSL>* asyncSocket = (AsyncSocket<SSL>*)httpContextData->upgradedWebSocket;

                auto [written, failed] = asyncSocket->uncork();

                if (!failed) {
                    WebSocketData* webSocketData = (WebSocketData*)asyncSocket->getAsyncSocketData();
                    if (webSocketData->isShuttingDown) {
                        asyncSocket->shutdown();
                    }
                }

                httpContextData->upgradedWebSocket = nullptr;

                return (us_socket_t*)asyncSocket;
            }

            ((AsyncSocket<SSL>*)s)->uncork();

            return s;
        });

        us_socket_context_on_writable(SSL, getSocketContext(), [](us_socket_t* s) {
            AsyncSocket<SSL>* asyncSocket = (AsyncSocket<SSL>*)s;
            HttpResponseData<SSL>* httpResponseData = (HttpResponseData<SSL>*)asyncSocket->getAsyncSocketData();

            if (httpResponseData->onWritable) {
                us_socket_timeout(SSL, s, 0);

                bool success = httpResponseData->callOnWritable(httpResponseData->offset);

                if (!success) {
                    return s;
                }

                return s;
            }

            asyncSocket->write(nullptr, 0, true, 0);

            if (httpResponseData->state & HttpResponseData<SSL>::HTTP_CONNECTION_CLOSE) {
                if ((httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING) == 0) {
                    if (asyncSocket->getBufferedAmount() == 0) {
                        asyncSocket->shutdown();
                        asyncSocket->close();
                    }
                }
            }

            asyncSocket->timeout(HTTP_IDLE_TIMEOUT_S);

            return s;
        });

        us_socket_context_on_end(SSL, getSocketContext(), [](us_socket_t* s) {
            AsyncSocket<SSL>* asyncSocket = (AsyncSocket<SSL>*)s;
            return asyncSocket->close();
        });

        us_socket_context_on_timeout(SSL, getSocketContext(), [](us_socket_t* s) {
            AsyncSocket<SSL>* asyncSocket = (AsyncSocket<SSL>*)s;
            return asyncSocket->close();
        });

        return this;
    }

   public:
    static HttpContext* create(Loop* loop, us_socket_context_options_t options = {}) {
        HttpContext* httpContext;

        httpContext =
            (HttpContext*)us_create_socket_context(SSL, (us_loop_t*)loop, sizeof(HttpContextData<SSL>), options);

        if (!httpContext) {
            return nullptr;
        }

        new ((HttpContextData<SSL>*)us_socket_context_ext(SSL, (us_socket_context_t*)httpContext))
            HttpContextData<SSL>();
        return httpContext->init();
    }

    void free() {
        HttpContextData<SSL>* httpContextData = getSocketContextData();
        httpContextData->~HttpContextData<SSL>();

        us_socket_context_free(SSL, getSocketContext());
    }

    void filter(MoveOnlyFunction<void(HttpResponse<SSL>*, int)>&& filterHandler) {
        getSocketContextData()->filterHandlers.emplace_back(std::move(filterHandler));
    }

    void onHttp(std::string method, std::string pattern,
                MoveOnlyFunction<void(HttpResponse<SSL>*, HttpRequest*)>&& handler, bool upgrade = false) {
        HttpContextData<SSL>* httpContextData = getSocketContextData();

        std::vector<std::string> methods;
        if (method == "*") {
            methods = {"*"};
        } else {
            methods = {method};
        }

        uint32_t priority = method == "*" ? httpContextData->currentRouter->LOW_PRIORITY
                                          : (upgrade ? httpContextData->currentRouter->HIGH_PRIORITY
                                                     : httpContextData->currentRouter->MEDIUM_PRIORITY);

        if (!handler) {
            httpContextData->currentRouter->remove(methods[0], pattern, priority);
            return;
        }

        std::map<std::string, unsigned short, std::less<>> parameterOffsets;
        unsigned short offset = 0;
        for (unsigned int i = 0; i < pattern.length(); i++) {
            if (pattern[i] == ':') {
                i++;
                unsigned int start = i;
                while (i < pattern.length() && pattern[i] != '/') {
                    i++;
                }
                parameterOffsets[std::string(pattern.data() + start, i - start)] = offset;
                offset++;
            }
        }

        httpContextData->currentRouter->add(
            methods, pattern,
            [handler = std::move(handler), parameterOffsets = std::move(parameterOffsets)](auto* r) mutable {
                auto user = r->getUserData();
                user.httpRequest->setYield(false);
                user.httpRequest->setParameters(r->getParameters());
                user.httpRequest->setParameterOffsets(&parameterOffsets);

                std::string_view expect = user.httpRequest->getHeader("expect");
                if (expect.length() && expect == "100-continue") {
                    user.httpResponse->writeContinue();
                }

                handler(user.httpResponse, user.httpRequest);

                if (user.httpRequest->getYield()) {
                    return false;
                }
                return true;
            },
            priority);
    }

    us_listen_socket_t* listen(const char* host, int port, int options) {
        return us_socket_context_listen(SSL, getSocketContext(), host, port, options, sizeof(HttpResponseData<SSL>));
    }

    us_listen_socket_t* listen(const char* path, int options) {
        return us_socket_context_listen_unix(SSL, getSocketContext(), path, options, sizeof(HttpResponseData<SSL>));
    }

    void onPreOpen(LIBUS_SOCKET_DESCRIPTOR (*handler)(struct us_socket_context_t*, LIBUS_SOCKET_DESCRIPTOR, char*,
                                                      int)) {
        us_socket_context_on_pre_open(SSL, getSocketContext(), handler);
    }

    us_socket_t* adoptAcceptedSocket(LIBUS_SOCKET_DESCRIPTOR accepted_fd, char* ip, int ip_length) {
        return us_adopt_accepted_socket(SSL, getSocketContext(), accepted_fd, sizeof(HttpResponseData<SSL>), ip,
                                        ip_length);
    }
};

}  // namespace uWS

#endif  // UWS_HTTPCONTEXT_H
