#pragma once

#include "mmltk/runtime/async_runtime.h"

#include <chrono>
#include <functional>
#include <thread>

namespace mmltk::testsupport {

inline void drain_until(mmltk::runtime::UiCallbackQueue& queue, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        queue.drain();
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    queue.drain();
}

}  
