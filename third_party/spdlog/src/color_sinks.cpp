
#ifndef SPDLOG_COMPILED_LIB
#error Please define SPDLOG_COMPILED_LIB to compile this file.
#endif

#include <spdlog/async.h>
#include <spdlog/details/null_mutex.h>

#include <mutex>

#include "spdlog/sinks/ansicolor_sink-inl.h"
#include "spdlog/sinks/stdout_color_sinks-inl.h"

// Explicit instantiations: one stamp per sink template / factory function.
#define SPDLOG_INSTANTIATE_CONSOLE_SINK(sink_template)                                      \
    template class SPDLOG_API spdlog::sinks::sink_template<spdlog::details::console_mutex>; \
    template class SPDLOG_API spdlog::sinks::sink_template<spdlog::details::console_nullmutex>;

SPDLOG_INSTANTIATE_CONSOLE_SINK(ansicolor_sink)
SPDLOG_INSTANTIATE_CONSOLE_SINK(ansicolor_stdout_sink)
SPDLOG_INSTANTIATE_CONSOLE_SINK(ansicolor_stderr_sink)
#undef SPDLOG_INSTANTIATE_CONSOLE_SINK

#define SPDLOG_INSTANTIATE_COLOR_LOGGER_FACTORY(func_name)                                              \
    template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::func_name<spdlog::synchronous_factory>( \
        const std::string& logger_name, color_mode mode);                                               \
    template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::func_name<spdlog::async_factory>(       \
        const std::string& logger_name, color_mode mode);

SPDLOG_INSTANTIATE_COLOR_LOGGER_FACTORY(stdout_color_mt)
SPDLOG_INSTANTIATE_COLOR_LOGGER_FACTORY(stdout_color_st)
SPDLOG_INSTANTIATE_COLOR_LOGGER_FACTORY(stderr_color_mt)
SPDLOG_INSTANTIATE_COLOR_LOGGER_FACTORY(stderr_color_st)
#undef SPDLOG_INSTANTIATE_COLOR_LOGGER_FACTORY
