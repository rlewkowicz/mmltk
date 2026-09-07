
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/backtracer.h>
#include <spdlog/details/log_msg.h>

#include <vector>

// X-macro over the log levels: X(method_name, level_enum_value). Used to stamp
// out the per-level logging surface once per overload family (logger members
// here, the global forwarders in spdlog.h). Deliberately not #undef'ed - it is
// part of spdlog's shared header-level helpers, like SPDLOG_LOGGER_CATCH.
#define SPDLOG_APPLY_LOG_LEVELS(X) \
    X(trace, spdlog::level::trace) \
    X(debug, spdlog::level::debug) \
    X(info, spdlog::level::info)   \
    X(warn, spdlog::level::warn)   \
    X(error, spdlog::level::err)   \
    X(critical, spdlog::level::critical)

#ifndef SPDLOG_NO_EXCEPTIONS
#define SPDLOG_LOGGER_CATCH(location)                                                                            \
    catch (const std::exception& ex) {                                                                           \
        if (location.filename) {                                                                                 \
            err_handler_(                                                                                        \
                fmt_lib::format(SPDLOG_FMT_STRING("{} [{}({})]"), ex.what(), location.filename, location.line)); \
        } else {                                                                                                 \
            err_handler_(ex.what());                                                                             \
        }                                                                                                        \
    }                                                                                                            \
    catch (...) {                                                                                                \
        err_handler_("Rethrowing unknown exception in logger");                                                  \
        throw;                                                                                                   \
    }
#else
#define SPDLOG_LOGGER_CATCH(location)
#endif

namespace spdlog {

class SPDLOG_API logger {
   public:
    explicit logger(std::string name) : name_(std::move(name)), sinks_() {}

    template <typename It>
    logger(std::string name, It begin, It end) : name_(std::move(name)), sinks_(begin, end) {}

    logger(std::string name, sink_ptr single_sink) : logger(std::move(name), {std::move(single_sink)}) {}

    logger(std::string name, sinks_init_list sinks) : logger(std::move(name), sinks.begin(), sinks.end()) {}

    virtual ~logger() = default;

    logger(const logger& other);
    logger(logger&& other) SPDLOG_NOEXCEPT;
    logger& operator=(logger other) SPDLOG_NOEXCEPT;
    void swap(spdlog::logger& other) SPDLOG_NOEXCEPT;

    template <typename... Args>
    void log(const source_loc& loc, level::level_enum lvl, format_string_t<Args...> fmt, Args&&... args) {
#if defined(SPDLOG_USE_STD_FORMAT) && __cpp_lib_format < 202207L
        log_(loc, lvl, fmt, std::forward<Args>(args)...);
#else
        log_(loc, lvl, fmt.get(), std::forward<Args>(args)...);
#endif
    }

    template <typename... Args>
    void log(level::level_enum lvl, format_string_t<Args...> fmt, Args&&... args) {
        log(source_loc{}, lvl, fmt, std::forward<Args>(args)...);
    }

    template <typename T>
    void log(level::level_enum lvl, const T& msg) {
        log(source_loc{}, lvl, msg);
    }

    template <class T, typename std::enable_if<!is_convertible_to_any_format_string<const T&>::value, int>::type = 0>
    void log(const source_loc& loc, level::level_enum lvl, const T& msg) {
        log(loc, lvl, "{}", msg);
    }

    void log(log_clock::time_point log_time, const source_loc& loc, level::level_enum lvl, string_view_t msg) {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled) {
            return;
        }

        details::log_msg log_msg(log_time, loc, name_, lvl, msg);
        log_it_(log_msg, log_enabled, traceback_enabled);
    }

    void log(const source_loc& loc, level::level_enum lvl, string_view_t msg) {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled) {
            return;
        }

        details::log_msg log_msg(loc, name_, lvl, msg);
        log_it_(log_msg, log_enabled, traceback_enabled);
    }

    void log(level::level_enum lvl, string_view_t msg) {
        log(source_loc{}, lvl, msg);
    }

    // Per-level convenience members, one overload family per stamp.
#define SPDLOG_LOGGER_LEVEL_FMT_FUNC(method, log_level)         \
    template <typename... Args>                                 \
    void method(format_string_t<Args...> fmt, Args&&... args) { \
        log(log_level, fmt, std::forward<Args>(args)...);       \
    }
    SPDLOG_APPLY_LOG_LEVELS(SPDLOG_LOGGER_LEVEL_FMT_FUNC)
#undef SPDLOG_LOGGER_LEVEL_FMT_FUNC

#define SPDLOG_LOGGER_LEVEL_MSG_FUNC(method, log_level) \
    template <typename T>                               \
    void method(const T& msg) {                         \
        log(log_level, msg);                            \
    }
    SPDLOG_APPLY_LOG_LEVELS(SPDLOG_LOGGER_LEVEL_MSG_FUNC)
#undef SPDLOG_LOGGER_LEVEL_MSG_FUNC

    bool should_log(level::level_enum msg_level) const {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    bool should_backtrace() const {
        return tracer_.enabled();
    }

    void set_level(level::level_enum log_level);

    level::level_enum level() const;

    const std::string& name() const;

    void set_formatter(std::unique_ptr<formatter> f);

    void set_pattern(std::string pattern, pattern_time_type time_type = pattern_time_type::local);

    void enable_backtrace(size_t n_messages);
    void disable_backtrace();
    void dump_backtrace();

    void flush();
    void flush_on(level::level_enum log_level);
    level::level_enum flush_level() const;

    const std::vector<sink_ptr>& sinks() const;

    std::vector<sink_ptr>& sinks();

    void set_error_handler(err_handler);

    virtual std::shared_ptr<logger> clone(std::string logger_name);

   protected:
    std::string name_;
    std::vector<sink_ptr> sinks_;
    spdlog::level_t level_{level::info};
    spdlog::level_t flush_level_{level::off};
    err_handler custom_err_handler_{nullptr};
    details::backtracer tracer_;

    template <typename... Args>
    void log_(const source_loc& loc, level::level_enum lvl, string_view_t fmt, Args&&... args) {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled) {
            return;
        }
        SPDLOG_TRY {
            memory_buf_t buf;
#ifdef SPDLOG_USE_STD_FORMAT
            fmt_lib::vformat_to(std::back_inserter(buf), fmt, fmt_lib::make_format_args(args...));
#else
            fmt::vformat_to(fmt::appender(buf), fmt, fmt::make_format_args(args...));
#endif

            details::log_msg log_msg(loc, name_, lvl, string_view_t(buf.data(), buf.size()));
            log_it_(log_msg, log_enabled, traceback_enabled);
        }
        SPDLOG_LOGGER_CATCH(loc)
    }

    void log_it_(const details::log_msg& log_msg, bool log_enabled, bool traceback_enabled);
    virtual void sink_it_(const details::log_msg& msg);
    virtual void flush_();
    void dump_backtrace_();
    bool should_flush_(const details::log_msg& msg) const;

    void err_handler_(const std::string& msg) const;
};

void swap(logger& a, logger& b) noexcept;

}

#ifdef SPDLOG_HEADER_ONLY
#include "logger-inl.h"
#endif
