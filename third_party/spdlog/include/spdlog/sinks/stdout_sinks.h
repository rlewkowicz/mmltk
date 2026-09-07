
#pragma once

#include <spdlog/details/console_globals.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/sinks/formatted_console_sink.h>

#include <cstdio>

namespace spdlog {

namespace sinks {

template <typename ConsoleMutex>
class stdout_sink_base : public formatted_console_sink<ConsoleMutex> {
   public:
    ~stdout_sink_base() override = default;

    void log(const details::log_msg& msg) override;

   protected:
    using formatted_console_sink<ConsoleMutex>::formatted_console_sink;
};

template <typename ConsoleMutex>
class stdout_sink : public stdout_sink_base<ConsoleMutex> {
   public:
    stdout_sink();
};

template <typename ConsoleMutex>
class stderr_sink : public stdout_sink_base<ConsoleMutex> {
   public:
    stderr_sink();
};

using stdout_sink_mt = stdout_sink<details::console_mutex>;
using stdout_sink_st = stdout_sink<details::console_nullmutex>;

using stderr_sink_mt = stderr_sink<details::console_mutex>;
using stderr_sink_st = stderr_sink<details::console_nullmutex>;

}

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stdout_logger_mt(const std::string& logger_name);

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stdout_logger_st(const std::string& logger_name);

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stderr_logger_mt(const std::string& logger_name);

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> stderr_logger_st(const std::string& logger_name);

}

#ifdef SPDLOG_HEADER_ONLY
#include "stdout_sinks-inl.h"
#endif
