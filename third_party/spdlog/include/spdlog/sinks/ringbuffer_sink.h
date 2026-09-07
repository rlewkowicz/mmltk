
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "spdlog/details/circular_q.h"
#include "spdlog/details/log_msg_buffer.h"
#include "spdlog/details/null_mutex.h"
#include "spdlog/sinks/base_sink.h"

namespace spdlog {
namespace sinks {
template <typename Mutex>
class ringbuffer_sink final : public base_sink<Mutex> {
   public:
    explicit ringbuffer_sink(size_t n_items) : q_{n_items} {
        if (n_items == 0) {
            throw_spdlog_ex("ringbuffer_sink: n_items cannot be zero");
        }
    }

    std::vector<details::log_msg_buffer> last_raw(size_t lim = 0) {
        return last_<details::log_msg_buffer>(lim, [](const details::log_msg_buffer& item) { return item; });
    }

    std::vector<std::string> last_formatted(size_t lim = 0) {
        return last_<std::string>(lim, [this](const details::log_msg_buffer& item) {
            memory_buf_t formatted;
            base_sink<Mutex>::formatter_->format(item, formatted);
            return SPDLOG_BUF_TO_STRING(formatted);
        });
    }

   protected:
    void sink_it_(const details::log_msg& msg) override {
        q_.push_back(details::log_msg_buffer{msg});
    }
    void flush_() override {}

   private:
    // Shared drain for the last_* accessors: locks, takes the newest n items
    // and materializes each through the supplied transform.
    template <typename T, typename Transform>
    std::vector<T> last_(size_t lim, Transform&& transform) {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        auto items_available = q_.size();
        auto n_items = lim > 0 ? (std::min)(lim, items_available) : items_available;
        std::vector<T> ret;
        ret.reserve(n_items);
        for (size_t i = (items_available - n_items); i < items_available; i++) {
            ret.push_back(transform(q_.at(i)));
        }
        return ret;
    }

    details::circular_q<details::log_msg_buffer> q_;
};

using ringbuffer_sink_mt = ringbuffer_sink<std::mutex>;
using ringbuffer_sink_st = ringbuffer_sink<details::null_mutex>;

}

}
