
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/periodic_file_helpers.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/base_sink.h>

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
class daily_file_sink final : public base_sink<Mutex> {
   public:
    daily_file_sink(filename_t base_filename, int rotation_hour, int rotation_minute, bool truncate = false,
                    uint16_t max_files = 0, const file_event_handlers& event_handlers = {})
        : base_filename_(std::move(base_filename)),
          rotation_h_(rotation_hour),
          rotation_m_(rotation_minute),
          file_helper_{event_handlers},
          truncate_(truncate),
          max_files_(max_files),
          filenames_q_() {
        if (rotation_hour < 0 || rotation_hour > 23 || rotation_minute < 0 || rotation_minute > 59) {
            throw_spdlog_ex("daily_file_sink: Invalid rotation time in ctor");
        }

        auto now = log_clock::now();
        const auto new_filename = FileNameCalc::calc_filename(base_filename_, detail::periodic_now_tm(now));
        file_helper_.open(new_filename, truncate_);
        rotation_tp_ = next_rotation_tp_();

        if (max_files_ > 0) {
            filenames_q_ =
                detail::init_periodic_filenames_q<FileNameCalc>(base_filename_, max_files_, std::chrono::hours(24));
        }
    }

    filename_t filename() {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        return file_helper_.filename();
    }

   protected:
    void sink_it_(const details::log_msg& msg) override {
        auto time = msg.time;
        bool should_rotate = time >= rotation_tp_;
        if (should_rotate) {
            const auto new_filename = FileNameCalc::calc_filename(base_filename_, detail::periodic_now_tm(time));
            file_helper_.open(new_filename, truncate_);
            rotation_tp_ = next_rotation_tp_();
        }
        details::write_formatted_log(file_helper_, base_sink<Mutex>::formatter_, msg);

        if (should_rotate && max_files_ > 0) {
            delete_old_();
        }
    }

    void flush_() override {
        file_helper_.flush();
    }

   private:
    log_clock::time_point next_rotation_tp_() {
        return detail::next_periodic_rotation_tp(std::chrono::hours(24), [this](tm& date) {
            date.tm_hour = rotation_h_;
            date.tm_min = rotation_m_;
            date.tm_sec = 0;
        });
    }

    void delete_old_() {
        detail::delete_old_periodic_file(file_helper_, filenames_q_, "daily");
    }

    filename_t base_filename_;
    int rotation_h_;
    int rotation_m_;
    log_clock::time_point rotation_tp_;
    details::file_helper file_helper_;
    bool truncate_;
    uint16_t max_files_;
    details::circular_q<filename_t> filenames_q_;
};

using daily_file_sink_mt = daily_file_sink<std::mutex>;
using daily_file_sink_st = daily_file_sink<details::null_mutex>;
using daily_file_format_sink_mt = daily_file_sink<std::mutex, daily_filename_format_calculator>;
using daily_file_format_sink_st = daily_file_sink<details::null_mutex, daily_filename_format_calculator>;

}  

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_mt(const std::string& logger_name, const filename_t& filename, int hour = 0,
                                               int minute = 0, bool truncate = false, uint16_t max_files = 0,
                                               const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::daily_file_sink_mt>(logger_name, filename, hour, minute, truncate, max_files,
                                                               event_handlers);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_format_mt(const std::string& logger_name, const filename_t& filename,
                                                      int hour = 0, int minute = 0, bool truncate = false,
                                                      uint16_t max_files = 0,
                                                      const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::daily_file_format_sink_mt>(logger_name, filename, hour, minute, truncate,
                                                                      max_files, event_handlers);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_st(const std::string& logger_name, const filename_t& filename, int hour = 0,
                                               int minute = 0, bool truncate = false, uint16_t max_files = 0,
                                               const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::daily_file_sink_st>(logger_name, filename, hour, minute, truncate, max_files,
                                                               event_handlers);
}

template <typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_format_st(const std::string& logger_name, const filename_t& filename,
                                                      int hour = 0, int minute = 0, bool truncate = false,
                                                      uint16_t max_files = 0,
                                                      const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::daily_file_format_sink_st>(logger_name, filename, hour, minute, truncate,
                                                                      max_files, event_handlers);
}
}  
