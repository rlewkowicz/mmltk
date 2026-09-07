
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/os.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/periodic_file_helpers.h>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace spdlog {
namespace sinks {

struct daily_filename_calculator {
    static filename_t calc_filename(const filename_t& filename, const tm& now_tm) {
        filename_t basename, ext;
        std::tie(basename, ext) = details::file_helper::split_by_extension(filename);
        return fmt_lib::format(SPDLOG_FMT_STRING(SPDLOG_FILENAME_T("{}_{:04d}-{:02d}-{:02d}{}")), basename,
                               now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday, ext);
    }
};

struct daily_filename_format_calculator {
    static filename_t calc_filename(const filename_t& file_path, const tm& now_tm) {
#if defined(_WIN32) && defined(SPDLOG_WCHAR_FILENAMES)
        std::wstringstream stream;
#else
        std::stringstream stream;
#endif
        stream << std::put_time(&now_tm, file_path.c_str());
        return stream.str();
    }
};

template <typename Mutex, typename FileNameCalc = daily_filename_calculator>
class daily_file_sink final
    : public detail::periodic_file_sink_base<Mutex, FileNameCalc, daily_file_sink<Mutex, FileNameCalc>> {
    using base_t = detail::periodic_file_sink_base<Mutex, FileNameCalc, daily_file_sink<Mutex, FileNameCalc>>;
    friend base_t;

   public:
    daily_file_sink(filename_t base_filename, int rotation_hour, int rotation_minute, bool truncate = false,
                    uint16_t max_files = 0, const file_event_handlers& event_handlers = {})
        : base_t(std::move(base_filename), truncate, max_files, event_handlers),
          rotation_h_(rotation_hour),
          rotation_m_(rotation_minute) {
        if (rotation_hour < 0 || rotation_hour > 23 || rotation_minute < 0 || rotation_minute > 59) {
            throw_spdlog_ex("daily_file_sink: Invalid rotation time in ctor");
        }
        this->init_periodic_sink_();
    }

   private:
    static constexpr std::chrono::hours rotation_step() {
        return std::chrono::hours(24);
    }

    static constexpr const char* sink_kind() {
        return "daily";
    }

    log_clock::time_point next_rotation_tp_() {
        return detail::next_periodic_rotation_tp(rotation_step(), [this](tm& date) {
            date.tm_hour = rotation_h_;
            date.tm_min = rotation_m_;
            date.tm_sec = 0;
        });
    }

    int rotation_h_;
    int rotation_m_;
};

using daily_file_sink_mt = daily_file_sink<std::mutex>;
using daily_file_sink_st = daily_file_sink<details::null_mutex>;
using daily_file_format_sink_mt = daily_file_sink<std::mutex, daily_filename_format_calculator>;
using daily_file_format_sink_st = daily_file_sink<details::null_mutex, daily_filename_format_calculator>;

}

// Factory functions for the daily sink variants; one stamp per sink type.
#define SPDLOG_DAILY_LOGGER_FACTORY_FUNC(func_name, sink_type)                                                         \
    template <typename Factory = spdlog::synchronous_factory>                                                          \
    inline std::shared_ptr<logger> func_name(const std::string& logger_name, const filename_t& filename, int hour = 0, \
                                             int minute = 0, bool truncate = false, uint16_t max_files = 0,            \
                                             const file_event_handlers& event_handlers = {}) {                         \
        return Factory::template create<sinks::sink_type>(logger_name, filename, hour, minute, truncate, max_files,    \
                                                          event_handlers);                                             \
    }

SPDLOG_DAILY_LOGGER_FACTORY_FUNC(daily_logger_mt, daily_file_sink_mt)
SPDLOG_DAILY_LOGGER_FACTORY_FUNC(daily_logger_format_mt, daily_file_format_sink_mt)
SPDLOG_DAILY_LOGGER_FACTORY_FUNC(daily_logger_st, daily_file_sink_st)
SPDLOG_DAILY_LOGGER_FACTORY_FUNC(daily_logger_format_st, daily_file_format_sink_st)
#undef SPDLOG_DAILY_LOGGER_FACTORY_FUNC
}
