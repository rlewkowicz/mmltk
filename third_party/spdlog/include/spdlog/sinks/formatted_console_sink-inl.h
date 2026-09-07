
#pragma once

#ifndef SPDLOG_HEADER_ONLY
#include <spdlog/sinks/formatted_console_sink.h>
#endif

#include <spdlog/pattern_formatter.h>

#include <memory>
#include <mutex>
#include <utility>

namespace spdlog {
namespace sinks {

template <typename ConsoleMutex>
SPDLOG_INLINE formatted_console_sink<ConsoleMutex>::formatted_console_sink(FILE* file)
    : file_(file), mutex_(ConsoleMutex::mutex()), formatter_(details::make_unique<spdlog::pattern_formatter>()) {}

template <typename ConsoleMutex>
SPDLOG_INLINE void formatted_console_sink<ConsoleMutex>::flush() {
    std::lock_guard<mutex_t> lock(mutex_);
    fflush(file_);
}

template <typename ConsoleMutex>
SPDLOG_INLINE void formatted_console_sink<ConsoleMutex>::set_pattern(const std::string& pattern) {
    std::lock_guard<mutex_t> lock(mutex_);
    formatter_ = std::unique_ptr<spdlog::formatter>(new pattern_formatter(pattern));
}

template <typename ConsoleMutex>
SPDLOG_INLINE void formatted_console_sink<ConsoleMutex>::set_formatter(
    std::unique_ptr<spdlog::formatter> sink_formatter) {
    std::lock_guard<mutex_t> lock(mutex_);
    formatter_ = std::move(sink_formatter);
}

}
}
