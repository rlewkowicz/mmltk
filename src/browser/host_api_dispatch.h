#pragma once

#include "browser/host_api_intents.h"

#include <variant>

namespace mmltk::browser {

template <typename PayloadTypeList, typename HandlerSet>
struct BrowserIntentDispatchFactory {
    template <typename Handler>
    static decltype(auto) dispatch(const RoutedIntent& intent, Handler&& handlers) {
        return std::visit([&](const auto& payload) -> decltype(auto) { return handlers(intent, payload); },
                          intent.payload);
    }
};

template <typename HandlerSet>
using BrowserIntentHandlerSet = HandlerSet;

}  
