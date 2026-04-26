// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#ifndef SPDLOG_H
#define SPDLOG_H

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/registry.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/logger.h>
#include <spdlog/version.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace spdlog {

using default_factory = synchronous_factory;

template <typename Sink, typename... SinkArgs>
inline std::shared_ptr<spdlog::logger> create(std::string logger_name, SinkArgs&&... sink_args) {
    return default_factory::create<Sink>(std::move(logger_name), std::forward<SinkArgs>(sink_args)...);
}

SPDLOG_API void initialize_logger(std::shared_ptr<logger> logger);

SPDLOG_API std::shared_ptr<logger> get(const std::string& name);

SPDLOG_API void set_formatter(std::unique_ptr<spdlog::formatter> formatter);

SPDLOG_API void set_pattern(std::string pattern, pattern_time_type time_type = pattern_time_type::local);

SPDLOG_API void enable_backtrace(size_t n_messages);

SPDLOG_API void disable_backtrace();

SPDLOG_API void dump_backtrace();

SPDLOG_API level::level_enum get_level();

SPDLOG_API void set_level(level::level_enum log_level);

SPDLOG_API bool should_log(level::level_enum log_level);

SPDLOG_API void flush_on(level::level_enum log_level);

template <typename Rep, typename Period>
inline void flush_every(std::chrono::duration<Rep, Period> interval) {
    details::registry::instance().flush_every(interval);
}

SPDLOG_API void set_error_handler(void (*handler)(const std::string& msg));

SPDLOG_API void register_logger(std::shared_ptr<logger> logger);

SPDLOG_API void register_or_replace(std::shared_ptr<logger> logger);

SPDLOG_API void apply_all(const std::function<void(std::shared_ptr<logger>)>& fun);

SPDLOG_API void drop(const std::string& name);

SPDLOG_API void drop_all();

SPDLOG_API void shutdown();

SPDLOG_API void set_automatic_registration(bool automatic_registration);

SPDLOG_API std::shared_ptr<spdlog::logger> default_logger();

SPDLOG_API spdlog::logger* default_logger_raw();

SPDLOG_API void set_default_logger(std::shared_ptr<spdlog::logger> default_logger);

SPDLOG_API void apply_logger_env_levels(std::shared_ptr<logger> logger);

template <typename... Args>
inline void log(source_loc source, level::level_enum lvl, format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->log(source, lvl, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log(level::level_enum lvl, format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->log(source_loc{}, lvl, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void trace(format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->trace(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void debug(format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->error(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void critical(format_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->critical(fmt, std::forward<Args>(args)...);
}

template <typename T>
inline void log(source_loc source, level::level_enum lvl, const T& msg) {
    default_logger_raw()->log(source, lvl, msg);
}

template <typename T>
inline void log(level::level_enum lvl, const T& msg) {
    default_logger_raw()->log(lvl, msg);
}

#ifdef SPDLOG_WCHAR_TO_UTF8_SUPPORT
template <typename... Args>
inline void log(source_loc source, level::level_enum lvl, wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->log(source, lvl, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log(level::level_enum lvl, wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->log(source_loc{}, lvl, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void trace(wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->trace(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void debug(wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->error(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void critical(wformat_string_t<Args...> fmt, Args&&... args) {
    default_logger_raw()->critical(fmt, std::forward<Args>(args)...);
}
#endif

template <typename T>
inline void trace(const T& msg) {
    default_logger_raw()->trace(msg);
}

template <typename T>
inline void debug(const T& msg) {
    default_logger_raw()->debug(msg);
}

template <typename T>
inline void info(const T& msg) {
    default_logger_raw()->info(msg);
}

template <typename T>
inline void warn(const T& msg) {
    default_logger_raw()->warn(msg);
}

template <typename T>
inline void error(const T& msg) {
    default_logger_raw()->error(msg);
}

template <typename T>
inline void critical(const T& msg) {
    default_logger_raw()->critical(msg);
}

}  // namespace spdlog

// SPDLOG_LEVEL_DEBUG,

#ifndef SPDLOG_NO_SOURCE_LOC
#define SPDLOG_LOGGER_CALL(logger, level, ...) \
    (logger)->log(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, level, __VA_ARGS__)
#else
#define SPDLOG_LOGGER_CALL(logger, level, ...) (logger)->log(spdlog::source_loc{}, level, __VA_ARGS__)
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
#define SPDLOG_LOGGER_TRACE(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::trace, __VA_ARGS__)
#define SPDLOG_TRACE(...) SPDLOG_LOGGER_TRACE(spdlog::default_logger_raw(), __VA_ARGS__)
#else
#define SPDLOG_LOGGER_TRACE(logger, ...) (void)0
#define SPDLOG_TRACE(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
#define SPDLOG_LOGGER_DEBUG(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::debug, __VA_ARGS__)
#define SPDLOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(spdlog::default_logger_raw(), __VA_ARGS__)
#else
#define SPDLOG_LOGGER_DEBUG(logger, ...) (void)0
#define SPDLOG_DEBUG(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
#define SPDLOG_LOGGER_INFO(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::info, __VA_ARGS__)
#define SPDLOG_INFO(...) SPDLOG_LOGGER_INFO(spdlog::default_logger_raw(), __VA_ARGS__)
#else
#define SPDLOG_LOGGER_INFO(logger, ...) (void)0
#define SPDLOG_INFO(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_WARN
#define SPDLOG_LOGGER_WARN(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::warn, __VA_ARGS__)
#define SPDLOG_WARN(...) SPDLOG_LOGGER_WARN(spdlog::default_logger_raw(), __VA_ARGS__)
#else
#define SPDLOG_LOGGER_WARN(logger, ...) (void)0
#define SPDLOG_WARN(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_ERROR
#define SPDLOG_LOGGER_ERROR(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::err, __VA_ARGS__)
#define SPDLOG_ERROR(...) SPDLOG_LOGGER_ERROR(spdlog::default_logger_raw(), __VA_ARGS__)
#else
#define SPDLOG_LOGGER_ERROR(logger, ...) (void)0
#define SPDLOG_ERROR(...) (void)0
#endif

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_CRITICAL
#define SPDLOG_LOGGER_CRITICAL(logger, ...) SPDLOG_LOGGER_CALL(logger, spdlog::level::critical, __VA_ARGS__)
#define SPDLOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(spdlog::default_logger_raw(), __VA_ARGS__)
#else
#define SPDLOG_LOGGER_CRITICAL(logger, ...) (void)0
#define SPDLOG_CRITICAL(...) (void)0
#endif

#ifdef SPDLOG_HEADER_ONLY
#include "spdlog-inl.h"
#endif

#endif  // SPDLOG_H
