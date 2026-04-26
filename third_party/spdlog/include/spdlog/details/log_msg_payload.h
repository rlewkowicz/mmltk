// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/formatter.h>

#include <limits>

namespace spdlog {
namespace details {

inline string_view_t format_log_msg_payload(const bool enable_formatting, formatter& sink_formatter, const log_msg& msg,
                                            memory_buf_t& formatted) {
    if (!enable_formatting) {
        return msg.payload;
    }

    formatted.clear();
    sink_formatter.format(msg, formatted);
    return string_view_t(formatted.data(), formatted.size());
}

inline int log_msg_payload_length(const string_view_t payload) {
    const size_t max_length = static_cast<size_t>(std::numeric_limits<int>::max());
    const size_t length = payload.size() > max_length ? max_length : payload.size();
    return static_cast<int>(length);
}

}  // namespace details
}  // namespace spdlog
