
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/os.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/periodic_file_helpers.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace spdlog {
namespace sinks {

struct hourly_filename_calculator {
    static filename_t calc_filename(const filename_t& filename, const tm& now_tm) {
        filename_t basename, ext;
        std::tie(basename, ext) = details::file_helper::split_by_extension(filename);
        return fmt_lib::format(SPDLOG_FILENAME_T("{}_{:04d}-{:02d}-{:02d}_{:02d}{}"), basename, now_tm.tm_year + 1900,
                               now_tm.tm_mon + 1, now_tm.tm_mday, now_tm.tm_hour, ext);
    }
};

template <typename Mutex, typename FileNameCalc = hourly_filename_calculator>
class hourly_file_sink final
    : public detail::periodic_file_sink_base<Mutex, FileNameCalc, hourly_file_sink<Mutex, FileNameCalc>> {
    using base_t = detail::periodic_file_sink_base<Mutex, FileNameCalc, hourly_file_sink<Mutex, FileNameCalc>>;
    friend base_t;

   public:
    hourly_file_sink(filename_t base_filename, bool truncate = false, uint16_t max_files = 0,
                     const file_event_handlers& event_handlers = {})
        : base_t(std::move(base_filename), truncate, max_files, event_handlers) {
        this->init_periodic_sink_();
        remove_init_file_ = this->file_helper_.size() == 0;
    }

   private:
    static constexpr std::chrono::hours rotation_step() {
        return std::chrono::hours(1);
    }

    static constexpr const char* sink_kind() {
        return "hourly";
    }

    log_clock::time_point next_rotation_tp_() {
        return detail::next_periodic_rotation_tp(rotation_step(), [](tm& date) {
            date.tm_min = 0;
            date.tm_sec = 0;
        });
    }

    void before_rotation_() {
        if (remove_init_file_) {
            this->file_helper_.close();
            details::os::remove(this->file_helper_.filename());
        }
    }

    void after_rotation_check_() {
        remove_init_file_ = false;
    }

    bool remove_init_file_ = false;
};

using hourly_file_sink_mt = hourly_file_sink<std::mutex>;
using hourly_file_sink_st = hourly_file_sink<details::null_mutex>;

}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> hourly_logger_mt(const std::string& logger_name, const filename_t& filename,
                                                bool truncate = false, uint16_t max_files = 0,
                                                const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::hourly_file_sink_mt>(logger_name, filename, truncate, max_files,
                                                                event_handlers);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> hourly_logger_st(const std::string& logger_name, const filename_t& filename,
                                                bool truncate = false, uint16_t max_files = 0,
                                                const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::hourly_file_sink_st>(logger_name, filename, truncate, max_files,
                                                                event_handlers);
}
}
