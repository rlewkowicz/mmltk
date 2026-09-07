
#pragma once

#include <spdlog/details/console_globals.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/formatted_console_sink.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>

namespace spdlog {
namespace sinks {

template <typename ConsoleMutex>
class ansicolor_sink : public formatted_console_sink<ConsoleMutex> {
   public:
    using mutex_t = typename ConsoleMutex::mutex_t;
    ansicolor_sink(FILE* target_file, color_mode mode);
    ~ansicolor_sink() override = default;

    void set_color(level::level_enum color_level, string_view_t color);
    void set_color_mode(color_mode mode);
    bool should_color() const;

    void log(const details::log_msg& msg) override;

    const string_view_t reset = "\033[m";
    const string_view_t bold = "\033[1m";
    const string_view_t dark = "\033[2m";
    const string_view_t underline = "\033[4m";
    const string_view_t blink = "\033[5m";
    const string_view_t reverse = "\033[7m";
    const string_view_t concealed = "\033[8m";
    const string_view_t clear_line = "\033[K";

    const string_view_t black = "\033[30m";
    const string_view_t red = "\033[31m";
    const string_view_t green = "\033[32m";
    const string_view_t yellow = "\033[33m";
    const string_view_t blue = "\033[34m";
    const string_view_t magenta = "\033[35m";
    const string_view_t cyan = "\033[36m";
    const string_view_t white = "\033[37m";

    const string_view_t on_black = "\033[40m";
    const string_view_t on_red = "\033[41m";
    const string_view_t on_green = "\033[42m";
    const string_view_t on_yellow = "\033[43m";
    const string_view_t on_blue = "\033[44m";
    const string_view_t on_magenta = "\033[45m";
    const string_view_t on_cyan = "\033[46m";
    const string_view_t on_white = "\033[47m";

    const string_view_t yellow_bold = "\033[33m\033[1m";
    const string_view_t red_bold = "\033[31m\033[1m";
    const string_view_t bold_on_red = "\033[1m\033[41m";

   private:
    bool should_do_colors_;
    std::array<std::string, level::n_levels> colors_;
    void set_color_mode_(color_mode mode);
    void print_ccode_(const string_view_t& color_code) const;
    void print_range_(const memory_buf_t& formatted, size_t start, size_t end) const;
    static std::string to_string_(const string_view_t& sv);
};

template <typename ConsoleMutex>
class ansicolor_stdout_sink : public ansicolor_sink<ConsoleMutex> {
   public:
    explicit ansicolor_stdout_sink(color_mode mode = color_mode::automatic);
};

template <typename ConsoleMutex>
class ansicolor_stderr_sink : public ansicolor_sink<ConsoleMutex> {
   public:
    explicit ansicolor_stderr_sink(color_mode mode = color_mode::automatic);
};

using ansicolor_stdout_sink_mt = ansicolor_stdout_sink<details::console_mutex>;
using ansicolor_stdout_sink_st = ansicolor_stdout_sink<details::console_nullmutex>;

using ansicolor_stderr_sink_mt = ansicolor_stderr_sink<details::console_mutex>;
using ansicolor_stderr_sink_st = ansicolor_stderr_sink<details::console_nullmutex>;

}
}

#ifdef SPDLOG_HEADER_ONLY
#include "ansicolor_sink-inl.h"
#endif
