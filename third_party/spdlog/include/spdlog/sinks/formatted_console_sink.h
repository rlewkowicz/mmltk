
#pragma once

#include <spdlog/sinks/sink.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace spdlog {
namespace sinks {

// Shared base for console sinks that own a FILE*, a shared console mutex and a
// formatter: implements the formatter/flush plumbing once.
template <typename ConsoleMutex>
class formatted_console_sink : public sink {
   public:
    using mutex_t = typename ConsoleMutex::mutex_t;

    formatted_console_sink(const formatted_console_sink& other) = delete;
    formatted_console_sink(formatted_console_sink&& other) = delete;

    formatted_console_sink& operator=(const formatted_console_sink& other) = delete;
    formatted_console_sink& operator=(formatted_console_sink&& other) = delete;

    void flush() override;
    void set_pattern(const std::string& pattern) override;
    void set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) override;

   protected:
    explicit formatted_console_sink(FILE* file);
    ~formatted_console_sink() override = default;

    FILE* file_;
    mutex_t& mutex_;
    std::unique_ptr<spdlog::formatter> formatter_;
};

}
}

#ifdef SPDLOG_HEADER_ONLY
#include "formatted_console_sink-inl.h"
#endif
