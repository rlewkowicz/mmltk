
#pragma once

#include <spdlog/details/log_msg.h>

namespace spdlog {
namespace details {

class SPDLOG_API log_msg_buffer : public log_msg {
    memory_buf_t buffer;
    void update_string_views();

   public:
    log_msg_buffer() = default;
    explicit log_msg_buffer(const log_msg& orig_msg);
    log_msg_buffer(const log_msg_buffer& other);
    log_msg_buffer(log_msg_buffer&& other) SPDLOG_NOEXCEPT;
    log_msg_buffer& operator=(const log_msg_buffer& other);
    log_msg_buffer& operator=(log_msg_buffer&& other) SPDLOG_NOEXCEPT;
};

}  
}  

#ifdef SPDLOG_HEADER_ONLY
#include "log_msg_buffer-inl.h"
#endif
