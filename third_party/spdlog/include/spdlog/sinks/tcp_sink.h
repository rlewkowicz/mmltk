// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#ifdef _WIN32
#include <spdlog/details/tcp_client-windows.h>
#else
#include <spdlog/details/tcp_client.h>
#endif

#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#pragma once

namespace spdlog {
namespace sinks {

struct tcp_sink_config {
    std::string server_host;
    int server_port;
    int timeout_ms = 0;
    bool lazy_connect = false;

    tcp_sink_config(std::string host, int port) : server_host{std::move(host)}, server_port{port} {}
};

template <typename Mutex>
class tcp_sink : public spdlog::sinks::base_sink<Mutex> {
   public:
    explicit tcp_sink(const std::string& host, int port, int timeout_ms = 0, bool lazy_connect = false)
        : config_{host, port} {
        config_.timeout_ms = timeout_ms;
        config_.lazy_connect = lazy_connect;
        if (!config_.lazy_connect) {
            client_.connect(config_.server_host, config_.server_port, config_.timeout_ms);
        }
    }

    explicit tcp_sink(tcp_sink_config sink_config) : config_{std::move(sink_config)} {
        if (!config_.lazy_connect) {
            client_.connect(config_.server_host, config_.server_port, config_.timeout_ms);
        }
    }

    ~tcp_sink() override = default;

   protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        if (!client_.is_connected()) {
            client_.connect(config_.server_host, config_.server_port, config_.timeout_ms);
        }
        client_.send(formatted.data(), formatted.size());
    }

    void flush_() override {}
    tcp_sink_config config_;
    details::tcp_client client_;
};

using tcp_sink_mt = tcp_sink<std::mutex>;
using tcp_sink_st = tcp_sink<spdlog::details::null_mutex>;

}  // namespace sinks
}  // namespace spdlog
