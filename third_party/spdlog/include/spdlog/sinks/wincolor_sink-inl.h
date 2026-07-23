
#pragma once

#ifndef SPDLOG_HEADER_ONLY
#include <spdlog/sinks/wincolor_sink.h>
#endif

#include <spdlog/details/color_log_formatter.h>
#include <spdlog/details/windows_include.h>
#include <wincon.h>

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>

namespace spdlog {
namespace sinks {
template <typename ConsoleMutex>
SPDLOG_INLINE wincolor_sink<ConsoleMutex>::wincolor_sink(void* out_handle, color_mode mode)
    : out_handle_(out_handle),
      mutex_(ConsoleMutex::mutex()),
      formatter_(details::make_unique<spdlog::pattern_formatter>()) {
    set_color_mode_impl(mode);
    colors_[level::trace] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    colors_[level::debug] = FOREGROUND_GREEN | FOREGROUND_BLUE;
    colors_[level::info] = FOREGROUND_GREEN;
    colors_[level::warn] = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    colors_[level::err] = FOREGROUND_RED | FOREGROUND_INTENSITY;
    colors_[level::critical] =
        BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    colors_[level::off] = 0;
}

template <typename ConsoleMutex>
SPDLOG_INLINE wincolor_sink<ConsoleMutex>::~wincolor_sink() {
    this->flush();
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::set_color(level::level_enum level, std::uint16_t color) {
    std::lock_guard<mutex_t> lock(mutex_);
    colors_[static_cast<size_t>(level)] = color;
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::log(const details::log_msg& msg) {
    if (out_handle_ == nullptr || out_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    details::with_color_formatted_log(mutex_, formatter_, msg, [this, &msg](const memory_buf_t& formatted) {
        if (should_do_colors_ && msg.color_range_end > msg.color_range_start) {
            print_range_(formatted, 0, msg.color_range_start);
            auto orig_attribs = static_cast<WORD>(set_foreground_color_(colors_[static_cast<size_t>(msg.level)]));
            print_range_(formatted, msg.color_range_start, msg.color_range_end);
            ::SetConsoleTextAttribute(static_cast<HANDLE>(out_handle_), orig_attribs);
            print_range_(formatted, msg.color_range_end, formatted.size());
        } else {
            write_to_file_(formatted);
        }
    });
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::flush() {}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::set_pattern(const std::string& pattern) {
    std::lock_guard<mutex_t> lock(mutex_);
    formatter_ = std::unique_ptr<spdlog::formatter>(new pattern_formatter(pattern));
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) {
    std::lock_guard<mutex_t> lock(mutex_);
    formatter_ = std::move(sink_formatter);
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::set_color_mode(color_mode mode) {
    std::lock_guard<mutex_t> lock(mutex_);
    set_color_mode_impl(mode);
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::set_color_mode_impl(color_mode mode) {
    if (mode == color_mode::automatic) {
        DWORD console_mode;
        bool in_console = ::GetConsoleMode(static_cast<HANDLE>(out_handle_), &console_mode) != 0;
        should_do_colors_ = in_console;
    } else {
        should_do_colors_ = mode == color_mode::always ? true : false;
    }
}

template <typename ConsoleMutex>
std::uint16_t SPDLOG_INLINE wincolor_sink<ConsoleMutex>::set_foreground_color_(std::uint16_t attribs) {
    CONSOLE_SCREEN_BUFFER_INFO orig_buffer_info;
    if (!::GetConsoleScreenBufferInfo(static_cast<HANDLE>(out_handle_), &orig_buffer_info)) {
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    auto new_attribs = static_cast<WORD>(attribs) | (orig_buffer_info.wAttributes & 0xfff0);
    auto ignored = ::SetConsoleTextAttribute(static_cast<HANDLE>(out_handle_), static_cast<WORD>(new_attribs));
    (void)(ignored);
    return static_cast<std::uint16_t>(orig_buffer_info.wAttributes);
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::print_range_(const memory_buf_t& formatted, size_t start, size_t end) {
    if (end > start) {
#if defined(SPDLOG_UTF8_TO_WCHAR_CONSOLE)
        wmemory_buf_t wformatted;
        details::os::utf8_to_wstrbuf(string_view_t(formatted.data() + start, end - start), wformatted);
        auto size = static_cast<DWORD>(wformatted.size());
        auto ignored = ::WriteConsoleW(static_cast<HANDLE>(out_handle_), wformatted.data(), size, nullptr, nullptr);
#else
        auto size = static_cast<DWORD>(end - start);
        auto ignored =
            ::WriteConsoleA(static_cast<HANDLE>(out_handle_), formatted.data() + start, size, nullptr, nullptr);
#endif
        (void)(ignored);
    }
}

template <typename ConsoleMutex>
void SPDLOG_INLINE wincolor_sink<ConsoleMutex>::write_to_file_(const memory_buf_t& formatted) {
    auto size = static_cast<DWORD>(formatted.size());
    DWORD bytes_written = 0;
    auto ignored = ::WriteFile(static_cast<HANDLE>(out_handle_), formatted.data(), size, &bytes_written, nullptr);
    (void)(ignored);
}

template <typename ConsoleMutex>
SPDLOG_INLINE wincolor_stdout_sink<ConsoleMutex>::wincolor_stdout_sink(color_mode mode)
    : wincolor_sink<ConsoleMutex>(::GetStdHandle(STD_OUTPUT_HANDLE), mode) {}

template <typename ConsoleMutex>
SPDLOG_INLINE wincolor_stderr_sink<ConsoleMutex>::wincolor_stderr_sink(color_mode mode)
    : wincolor_sink<ConsoleMutex>(::GetStdHandle(STD_ERROR_HANDLE), mode) {}
}  
}  
