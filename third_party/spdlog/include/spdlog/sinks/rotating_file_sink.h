
#pragma once

#include <spdlog/sinks/file_sinks_common.h>

namespace spdlog {
namespace sinks {
template <typename Mutex>
class rotating_file_sink final : public base_sink<Mutex> {
   public:
    static constexpr size_t MaxFiles = 200000;
    rotating_file_sink(filename_t base_filename, std::size_t max_size, std::size_t max_files,
                       bool rotate_on_open = false, const file_event_handlers& event_handlers = {});
    static filename_t calc_filename(const filename_t& filename, std::size_t index);
    filename_t filename();
    void rotate_now();
    void set_max_size(std::size_t max_size);
    std::size_t get_max_size();
    void set_max_files(std::size_t max_files);
    std::size_t get_max_files();

   protected:
    void sink_it_(const details::log_msg& msg) override;
    void flush_() override;

   private:
    void rotate_();

    bool rename_file_(const filename_t& src_filename, const filename_t& target_filename);

    filename_t base_filename_;
    std::size_t max_size_;
    std::size_t max_files_;
    std::size_t current_size_;
    details::file_helper file_helper_;
};

using rotating_file_sink_mt = rotating_file_sink<std::mutex>;
using rotating_file_sink_st = rotating_file_sink<details::null_mutex>;

}  

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> rotating_logger_mt(const std::string& logger_name, const filename_t& filename,
                                           size_t max_file_size, size_t max_files, bool rotate_on_open = false,
                                           const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::rotating_file_sink_mt>(logger_name, filename, max_file_size, max_files,
                                                                  rotate_on_open, event_handlers);
}

template <typename Factory = spdlog::synchronous_factory>
std::shared_ptr<logger> rotating_logger_st(const std::string& logger_name, const filename_t& filename,
                                           size_t max_file_size, size_t max_files, bool rotate_on_open = false,
                                           const file_event_handlers& event_handlers = {}) {
    return Factory::template create<sinks::rotating_file_sink_st>(logger_name, filename, max_file_size, max_files,
                                                                  rotate_on_open, event_handlers);
}
}  

#ifdef SPDLOG_HEADER_ONLY
#include "rotating_file_sink-inl.h"
#endif
