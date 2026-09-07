
#pragma once

#ifndef SPDLOG_HEADER_ONLY
#include <spdlog/sinks/stdout_sinks.h>
#endif

#include <spdlog/details/console_globals.h>
#include <spdlog/details/os.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/formatted_console_sink-inl.h>

#include <memory>
#include <utility>

namespace spdlog {

namespace sinks {

template <typename ConsoleMutex>
SPDLOG_INLINE void stdout_sink_base<ConsoleMutex>::log(const details::log_msg& msg) {
    std::lock_guard<typename ConsoleMutex::mutex_t> lock(this->mutex_);
    memory_buf_t formatted;
    this->formatter_->format(msg, formatted);
    details::os::fwrite_bytes(formatted.data(), formatted.size(), this->file_);
    ::fflush(this->file_);
}

template <typename ConsoleMutex>
SPDLOG_INLINE stdout_sink<ConsoleMutex>::stdout_sink() : stdout_sink_base<ConsoleMutex>(stdout) {}

template <typename ConsoleMutex>
SPDLOG_INLINE stderr_sink<ConsoleMutex>::stderr_sink() : stdout_sink_base<ConsoleMutex>(stderr) {}

}

namespace details {

template <typename Factory, typename Sink>
SPDLOG_INLINE std::shared_ptr<logger> create_stdout_stream_logger(const std::string& logger_name) {
    return Factory::template create<Sink>(logger_name);
}

}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stdout_logger_mt(const std::string& logger_name) {
    return details::create_stdout_stream_logger<Factory, sinks::stdout_sink_mt>(logger_name);
}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stdout_logger_st(const std::string& logger_name) {
    return details::create_stdout_stream_logger<Factory, sinks::stdout_sink_st>(logger_name);
}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stderr_logger_mt(const std::string& logger_name) {
    return details::create_stdout_stream_logger<Factory, sinks::stderr_sink_mt>(logger_name);
}

template <typename Factory>
SPDLOG_INLINE std::shared_ptr<logger> stderr_logger_st(const std::string& logger_name) {
    return details::create_stdout_stream_logger<Factory, sinks::stderr_sink_st>(logger_name);
}
}
