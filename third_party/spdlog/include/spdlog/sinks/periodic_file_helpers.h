
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/base_sink.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace spdlog {
namespace sinks {
namespace detail {

template <typename FileNameCalc>
spdlog::details::circular_q<filename_t> init_periodic_filenames_q(const filename_t& base_filename,
                                                                  std::uint16_t max_files, std::chrono::hours step) {
    spdlog::details::circular_q<filename_t> filenames_q(static_cast<std::size_t>(max_files));
    std::vector<filename_t> filenames;
    auto now = log_clock::now();
    while (filenames.size() < max_files) {
        const auto new_filename =
            FileNameCalc::calc_filename(base_filename, spdlog::details::os::localtime(log_clock::to_time_t(now)));
        if (!spdlog::details::os::path_exists(new_filename)) {
            break;
        }
        filenames.emplace_back(new_filename);
        now -= step;
    }
    for (auto iter = filenames.rbegin(); iter != filenames.rend(); ++iter) {
        filenames_q.push_back(std::move(*iter));
    }
    return filenames_q;
}

inline tm periodic_now_tm(const log_clock::time_point tp) {
    return spdlog::details::os::localtime(log_clock::to_time_t(tp));
}

template <typename SetTm>
log_clock::time_point next_periodic_rotation_tp(std::chrono::hours step, SetTm&& set_tm) {
    auto now = log_clock::now();
    tm date = periodic_now_tm(now);
    set_tm(date);
    auto rotation_time = log_clock::from_time_t(std::mktime(&date));
    if (rotation_time > now) {
        return rotation_time;
    }
    return {rotation_time + step};
}

template <typename Queue>
void delete_old_periodic_file(details::file_helper& file_helper, Queue& filenames_q, const char* sink_name) {
    using details::os::filename_to_str;
    using details::os::remove_if_exists;

    filename_t current_file = file_helper.filename();
    if (filenames_q.full()) {
        auto old_filename = std::move(filenames_q.front());
        filenames_q.pop_front();
        const bool ok = remove_if_exists(old_filename) == 0;
        if (!ok) {
            filenames_q.push_back(std::move(current_file));
            SPDLOG_THROW(spdlog_ex(
                "Failed removing " + std::string(sink_name) + " file " + filename_to_str(old_filename), errno));
        }
    }
    filenames_q.push_back(std::move(current_file));
}

// CRTP base for sinks that rotate to a freshly calculated file name on a fixed
// period (daily/hourly). Owns the shared state and implements the common
// open/rotate/write/cleanup flow; the derived sink supplies the period, the
// sink kind name, the next-rotation calculation and optional rotation hooks.
template <typename Mutex, typename FileNameCalc, typename Derived>
class periodic_file_sink_base : public base_sink<Mutex> {
   public:
    filename_t filename() {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        return file_helper_.filename();
    }

   protected:
    periodic_file_sink_base(filename_t base_filename, bool truncate, std::uint16_t max_files,
                            const file_event_handlers& event_handlers)
        : base_filename_(std::move(base_filename)),
          file_helper_{event_handlers},
          truncate_(truncate),
          max_files_(max_files),
          filenames_q_() {}

    // Opens the initial file and primes the rotation schedule; called from
    // derived constructors once their own state is ready.
    void init_periodic_sink_() {
        auto now = log_clock::now();
        file_helper_.open(FileNameCalc::calc_filename(base_filename_, periodic_now_tm(now)), truncate_);
        rotation_tp_ = derived().next_rotation_tp_();

        if (max_files_ > 0) {
            filenames_q_ =
                init_periodic_filenames_q<FileNameCalc>(base_filename_, max_files_, Derived::rotation_step());
        }
    }

    void sink_it_(const spdlog::details::log_msg& msg) override {
        auto time = msg.time;
        const bool should_rotate = time >= rotation_tp_;
        if (should_rotate) {
            derived().before_rotation_();
            file_helper_.open(FileNameCalc::calc_filename(base_filename_, periodic_now_tm(time)), truncate_);
            rotation_tp_ = derived().next_rotation_tp_();
        }
        derived().after_rotation_check_();
        spdlog::details::write_formatted_log(file_helper_, base_sink<Mutex>::formatter_, msg);

        if (should_rotate && max_files_ > 0) {
            delete_old_periodic_file(file_helper_, filenames_q_, Derived::sink_kind());
        }
    }

    void flush_() override {
        file_helper_.flush();
    }

    // Default rotation hooks; derived sinks shadow these when needed.
    void before_rotation_() {}
    void after_rotation_check_() {}

    Derived& derived() {
        return static_cast<Derived&>(*this);
    }

    filename_t base_filename_;
    log_clock::time_point rotation_tp_;
    spdlog::details::file_helper file_helper_;
    bool truncate_;
    std::uint16_t max_files_;
    spdlog::details::circular_q<filename_t> filenames_q_;
};

}
}
}
