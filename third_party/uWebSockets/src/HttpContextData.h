
#ifndef UWS_HTTPCONTEXTDATA_H
#define UWS_HTTPCONTEXTDATA_H

#include "HttpRouter.h"

#include <vector>
#include "MoveOnlyFunction.h"

namespace uWS {
template <bool>
struct HttpResponse;
struct HttpRequest;

template <bool SSL>
struct alignas(16) HttpContextData {
    template <bool>
    friend struct HttpContext;
    template <bool>
    friend struct HttpResponse;
    template <bool>
    friend struct TemplatedApp;

   private:
    std::vector<MoveOnlyFunction<void(HttpResponse<SSL>*, int)>> filterHandlers;

    MoveOnlyFunction<void(const char* hostname)> missingServerNameHandler;

    struct RouterData {
        HttpResponse<SSL>* httpResponse;
        HttpRequest* httpRequest;
    };

    HttpRouter<RouterData>* currentRouter = &router;

    HttpRouter<RouterData> router;
    void* upgradedWebSocket = nullptr;
    bool isParsingHttp = false;

    std::vector<void*> childApps;
    unsigned int roundRobin = 0;
};

}  

#endif  // UWS_HTTPCONTEXTDATA_H
