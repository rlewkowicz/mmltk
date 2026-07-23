#ifndef UWS_CACHINGAPP_H
#define UWS_CACHINGAPP_H

#include "App.h"
#include <unordered_map>
#include <string>
#include <functional>
#include <string_view>

namespace uWS {

struct StringViewHash {
    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
};

struct StringViewEqual {
    bool operator()(std::string_view sv1, std::string_view sv2) const {
        return sv1 == sv2;
    }
};

class CachingHttpResponse {
   public:
    CachingHttpResponse(uWS::HttpResponse<false>* res) : res(res) {}

    void write(std::string_view data) {
        buffer.append(data);
    }

    void end(std::string_view data = "", bool closeConnection = false) {
        buffer.append(data);

        res->end(buffer);

        created = time(0);

        std::ignore = closeConnection;
    }

   public:
    uWS::HttpResponse<false>* res;

    std::string buffer;
    time_t created;
};

typedef std::unordered_map<std::string_view, CachingHttpResponse*, StringViewHash, StringViewEqual> CacheType;

template <bool SSL>
struct CachingApp : public uWS::TemplatedAppBase<SSL, CachingApp<SSL>> {
   public:
    CachingApp(SocketContextOptions options = {}) : uWS::TemplatedAppBase<SSL, CachingApp<SSL>>(options) {}

    using uWS::TemplatedAppBase<SSL, CachingApp<SSL>>::get;

    CachingApp(const CachingApp& other) = delete;
    CachingApp(CachingApp<SSL>&& other) : uWS::TemplatedAppBase<SSL, CachingApp<SSL>>(std::move(other)) {}

    ~CachingApp() {}

    CachingApp&& get(const std::string& url,
                     uWS::MoveOnlyFunction<void(CachingHttpResponse*, uWS::HttpRequest*)>&& handler,
                     unsigned int secondsToExpiry) {
        ((uWS::TemplatedAppBase<SSL, CachingApp<SSL>>*)this)
            ->get(url, [this, handler = std::move(handler), secondsToExpiry](auto* res, auto* req) mutable {
                std::string_view cache_key = req->getFullUrl();
                time_t now = static_cast<LoopData*>(us_loop_ext((us_loop_t*)uWS::Loop::get()))->cacheTimepoint;

                auto it = cache.find(cache_key);
                if (it != cache.end()) {
                    if (it->second->created + secondsToExpiry > now) {
                        res->end(it->second->buffer);
                        return;
                    }

                    delete it->second;
                }

                CachingHttpResponse* cachingRes;
                cache[cache_key] = (cachingRes = new CachingHttpResponse(res));

                handler(cachingRes, req);
            });
        return std::move(*this);
    }


   private:
    CacheType cache;
};

}  
#endif