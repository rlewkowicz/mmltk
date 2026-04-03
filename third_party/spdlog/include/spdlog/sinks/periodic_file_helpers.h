// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/os.h>

#include <cstddef>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace spdlog {
namespace sinks {
namespace detail {

template <typename FileNameCalc>
spdlog::details::circular_q<filename_t> init_periodic_filenames_q(const filename_t &base_filename,
                                                                  std::uint16_t max_files,
                                                                  std::chrono::hours step) {
    spdlog::details::circular_q<filename_t> filenames_q(
        static_cast<std::size_t>(max_files));
    std::vector<filename_t> filenames;
    auto now = log_clock::now();
    while (filenames.size() < max_files) {
        const auto new_filename = FileNameCalc::calc_filename(base_filename, spdlog::details::os::localtime(log_clock::to_time_t(now)));
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

template <typename Queue>
void delete_old_periodic_file(details::file_helper &file_helper,
                              Queue &filenames_q,
                              const char *sink_name) {
    using details::os::filename_to_str;
    using details::os::remove_if_exists;

    filename_t current_file = file_helper.filename();
    if (filenames_q.full()) {
        auto old_filename = std::move(filenames_q.front());
        filenames_q.pop_front();
        const bool ok = remove_if_exists(old_filename) == 0;
        if (!ok) {
            filenames_q.push_back(std::move(current_file));
            SPDLOG_THROW(spdlog_ex("Failed removing " + std::string(sink_name) + " file " +
                                   filename_to_str(old_filename),
                                   errno));
        }
    }
    filenames_q.push_back(std::move(current_file));
}

}  // namespace detail
}  // namespace sinks
}  // namespace spdlog
