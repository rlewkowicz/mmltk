
#ifndef SPDLOG_COMPILED_LIB
#error Please define SPDLOG_COMPILED_LIB to compile this file.
#endif

#include <spdlog/async.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/stdout_sinks-inl.h>

#include <mutex>

// Explicit instantiations: one stamp per sink template / factory function.
#define SPDLOG_INSTANTIATE_CONSOLE_SINK(sink_template)                                      \
    template class SPDLOG_API spdlog::sinks::sink_template<spdlog::details::console_mutex>; \
    template class SPDLOG_API spdlog::sinks::sink_template<spdlog::details::console_nullmutex>;

SPDLOG_INSTANTIATE_CONSOLE_SINK(stdout_sink_base)
SPDLOG_INSTANTIATE_CONSOLE_SINK(stdout_sink)
SPDLOG_INSTANTIATE_CONSOLE_SINK(stderr_sink)
#undef SPDLOG_INSTANTIATE_CONSOLE_SINK

#define SPDLOG_INSTANTIATE_STDOUT_LOGGER_FACTORY(func_name)                                             \
    template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::func_name<spdlog::synchronous_factory>( \
        const std::string& logger_name);                                                                \
    template SPDLOG_API std::shared_ptr<spdlog::logger> spdlog::func_name<spdlog::async_factory>(       \
        const std::string& logger_name);

SPDLOG_INSTANTIATE_STDOUT_LOGGER_FACTORY(stdout_logger_mt)
SPDLOG_INSTANTIATE_STDOUT_LOGGER_FACTORY(stdout_logger_st)
SPDLOG_INSTANTIATE_STDOUT_LOGGER_FACTORY(stderr_logger_mt)
SPDLOG_INSTANTIATE_STDOUT_LOGGER_FACTORY(stderr_logger_st)
#undef SPDLOG_INSTANTIATE_STDOUT_LOGGER_FACTORY
