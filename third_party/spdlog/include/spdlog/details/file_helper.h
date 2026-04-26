// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <tuple>

namespace spdlog {
namespace details {

class SPDLOG_API file_helper {
   public:
    file_helper() = default;
    explicit file_helper(const file_event_handlers& event_handlers);

    file_helper(const file_helper&) = delete;
    file_helper& operator=(const file_helper&) = delete;
    ~file_helper();

    void open(const filename_t& fname, bool truncate = false);
    void reopen(bool truncate);
    void flush();
    void sync();
    void close();
    void write(const memory_buf_t& buf);
    size_t size() const;
    const filename_t& filename() const;

    static std::tuple<filename_t, filename_t> split_by_extension(const filename_t& fname);

   private:
    const int open_tries_ = 5;
    const unsigned int open_interval_ = 10;
    std::FILE* fd_{nullptr};
    filename_t filename_;
    file_event_handlers event_handlers_;
};

template <typename FormatterPtr>
inline void write_formatted_log(file_helper& helper, FormatterPtr& formatter, const log_msg& msg) {
    memory_buf_t formatted;
    formatter->format(msg, formatted);
    helper.write(formatted);
}
}  // namespace details
}  // namespace spdlog

#ifdef SPDLOG_HEADER_ONLY
#include "file_helper-inl.h"
#endif
