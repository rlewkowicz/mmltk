// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>

#include <mutex>
#include <utility>

namespace spdlog {
namespace details {

template <typename Mutex, typename FormatterPtr, typename Callback>
void with_color_formatted_log(Mutex& mutex, FormatterPtr& formatter, const log_msg& msg, Callback&& callback) {
    std::lock_guard<Mutex> lock(mutex);
    msg.color_range_start = 0;
    msg.color_range_end = 0;
    memory_buf_t formatted;
    formatter->format(msg, formatted);
    callback(formatted);
}

}  // namespace details
}  // namespace spdlog
