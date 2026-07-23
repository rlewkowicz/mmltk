
#pragma once

#ifndef SPDLOG_HEADER_ONLY
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include <utility>

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace spdlog {

namespace details {

template <typename Factory, typename Sink, typename... Args>
SPDLOG_INLINE std::shared_ptr<logger> create_stdout_color_logger(const std::string& logger_name, Args&&... args) {
    return Factory::template create<Sink>(logger_name, std::forward<Args>(args)...);
}

}  

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stdout_color_mt(const std::string& logger_name, color_mode mode) {
    return details::create_stdout_color_logger<Factory, sinks::stdout_color_sink_mt>(logger_name, mode);
}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stdout_color_st(const std::string& logger_name, color_mode mode) {
    return details::create_stdout_color_logger<Factory, sinks::stdout_color_sink_st>(logger_name, mode);
}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stderr_color_mt(const std::string& logger_name, color_mode mode) {
    return details::create_stdout_color_logger<Factory, sinks::stderr_color_sink_mt>(logger_name, mode);
}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stderr_color_st(const std::string& logger_name, color_mode mode) {
    return details::create_stdout_color_logger<Factory, sinks::stderr_color_sink_st>(logger_name, mode);
}
}  
