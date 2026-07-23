#pragma once

#include "mmltk_logging.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mmltk::live {

inline void log_live_worker_message(const char* logger_name, const char* level, const std::string& message) {
    if (message.empty()) {
        return;
    }

    auto logger = mmltk::logging::logger(logger_name);
    if (std::string_view(level) == "error") {
        logger->error("{}", message);
    } else if (std::string_view(level) == "warn") {
        logger->warn("{}", message);
    } else {
        logger->info("{}", message);
    }
}

[[nodiscard]] inline std::uint64_t live_steady_clock_now_ns() noexcept {
    using Clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

[[nodiscard]] inline bool live_should_log_periodic_frame(const std::uint64_t count) noexcept {
    return count <= 3U || count % 300U == 0U;
}

}  
