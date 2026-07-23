
#pragma once

#ifndef SPDLOG_HEADER_ONLY
#include <spdlog/details/periodic_worker.h>
#endif

namespace spdlog {
namespace details {

SPDLOG_INLINE periodic_worker::~periodic_worker() {
    if (worker_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
        }
        cv_.notify_one();
        worker_thread_.join();
    }
}

}  
}  
