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

#ifndef UWS_HTTPRESPONSEDATA_H
#define UWS_HTTPRESPONSEDATA_H

#include "HttpParser.h"
#include "AsyncSocketData.h"
#include "ProxyParser.h"

#include "MoveOnlyFunction.h"

namespace uWS {

template <bool SSL>
struct HttpResponseData : AsyncSocketData<SSL>, HttpParser {
    template <bool>
    friend struct HttpResponse;
    template <bool>
    friend struct HttpContext;

    void markDone() {
        onAborted = nullptr;
        onWritable = nullptr;

        state &= ~HttpResponseData<SSL>::HTTP_RESPONSE_PENDING;
    }

    bool callOnWritable(uintmax_t offset) {
        MoveOnlyFunction<bool(uintmax_t)> borrowedOnWritable = std::move(onWritable);

        onWritable = [](uintmax_t) { return true; };

        bool ret = borrowedOnWritable(offset);

        if (onWritable) {
            onWritable = std::move(borrowedOnWritable);
        }

        return ret;
    }

   private:
    enum {
        HTTP_STATUS_CALLED = 1,
        HTTP_WRITE_CALLED = 2,
        HTTP_END_CALLED = 4,
        HTTP_RESPONSE_PENDING = 8,
        HTTP_CONNECTION_CLOSE = 16
    };

    MoveOnlyFunction<bool(uintmax_t)> onWritable;
    MoveOnlyFunction<void()> onAborted;
    MoveOnlyFunction<void(std::string_view, uint64_t)> inStream;
    uintmax_t offset = 0;

    unsigned int received_bytes_per_timeout = 0;

    int state = 0;

#ifdef UWS_WITH_PROXY
    ProxyParser proxyParser;
#endif
};

}  // namespace uWS

#endif  // UWS_HTTPRESPONSEDATA_H
