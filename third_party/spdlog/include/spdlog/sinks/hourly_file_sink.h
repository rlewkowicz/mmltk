
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/periodic_file_helpers.h>
#include <spdlog/details/synchronous_factory.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/base_sink.h>

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
class hourly_file_sink final : public base_sink<Mutex> {
   public:
    hourly_file_sink(filename_t base_filename, bool truncate = false, uint16_t max_files = 0,
                     const file_event_handlers& event_handlers = {})
        : base_filename_(std::move(base_filename)),
          file_helper_{event_handlers},
          truncate_(truncate),
          max_files_(max_files),
          filenames_q_() {
        auto now = log_clock::now();
        auto filename = FileNameCalc::calc_filename(base_filename_, detail::periodic_now_tm(now));
        file_helper_.open(filename, truncate_);
        remove_init_file_ = file_helper_.size() == 0;
        rotation_tp_ = next_rotation_tp_();

        if (max_files_ > 0) {
            filenames_q_ =
                detail::init_periodic_filenames_q<FileNameCalc>(base_filename_, max_files_, std::chrono::hours(1));
        }
    }

    filename_t filename() {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        return file_helper_.filename();
    }

   protected:
    void sink_it_(const details::log_msg& msg) override {
        auto time = msg.time;
        const bool should_rotate = time >= rotation_tp_;
        if (should_rotate) {
            if (remove_init_file_) {
                file_helper_.close();
                details::os::remove(file_helper_.filename());
            }
            auto filename = FileNameCalc::calc_filename(base_filename_, detail::periodic_now_tm(time));
            file_helper_.open(filename, truncate_);
            rotation_tp_ = next_rotation_tp_();
        }
        remove_init_file_ = false;
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
        return detail::next_periodic_rotation_tp(std::chrono::hours(1), [](tm& date) {
            date.tm_min = 0;
            date.tm_sec = 0;
        });
    }

    void delete_old_() {
        detail::delete_old_periodic_file(file_helper_, filenames_q_, "hourly");
    }

    filename_t base_filename_;
    log_clock::time_point rotation_tp_;
    details::file_helper file_helper_;
    bool truncate_;
    uint16_t max_files_;
    details::circular_q<filename_t> filenames_q_;
    bool remove_init_file_;
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
