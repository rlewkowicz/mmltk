#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>

#include <sys/ioctl.h>
#include <unistd.h>

namespace fastloader::rfdetr {

class ProgressBar {
public:
    ProgressBar(std::string label, size_t total, std::string unit)
        : label_(std::move(label)),
          total_(total),
          unit_(std::move(unit)),
          started_(std::chrono::steady_clock::now()),
          last_render_(started_) {}

    ~ProgressBar() {
        if (!closed_) {
            finalize(false);
        }
    }

    void add(size_t delta) {
        current_ += delta;
        render(false);
    }

    void set_total(size_t total) {
        total_ = total;
        render(true);
    }

    void set_postfix(std::string postfix) {
        postfix_ = std::move(postfix);
        render(true);
    }

    void close() {
        finalize(true);
    }

    static void log(const std::string& message) {
        std::fprintf(stderr, "\r\033[2K%s\n", message.c_str());
        std::fflush(stderr);
    }

private:
    void finalize(bool mark_complete) {
        if (closed_) {
            return;
        }
        if (mark_complete) {
            current_ = total_;
        }
        render(true);
        std::fputc('\n', stderr);
        std::fflush(stderr);
        closed_ = true;
    }

    void render(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && (now - last_render_) < std::chrono::milliseconds(100)) {
            return;
        }
        last_render_ = now;

        const double elapsed_seconds =
            std::chrono::duration<double>(now - started_).count();
        const double rate = elapsed_seconds > 0.0 ? static_cast<double>(current_) / elapsed_seconds : 0.0;
        const double progress =
            total_ > 0 ? std::clamp(static_cast<double>(current_) / static_cast<double>(total_), 0.0, 1.0) : 0.0;
        const size_t terminal_width = detect_terminal_width();

        std::string label = label_;
        bool include_postfix = !postfix_.empty();
        constexpr int kDefaultBarWidth = 32;
        constexpr int kMinimumBarWidth = 8;
        int bar_width = kDefaultBarWidth;

        std::string line = build_line(label, include_postfix, bar_width, progress, elapsed_seconds, rate);
        if (line.size() > terminal_width) {
            const std::string min_bar_line =
                build_line(label, include_postfix, kMinimumBarWidth, progress, elapsed_seconds, rate);
            const size_t fixed_width =
                min_bar_line.size() > static_cast<size_t>(kMinimumBarWidth)
                    ? min_bar_line.size() - static_cast<size_t>(kMinimumBarWidth)
                    : 0;
            const size_t available_bar = terminal_width > fixed_width ? terminal_width - fixed_width : 0;
            bar_width = static_cast<int>(std::clamp<size_t>(available_bar, kMinimumBarWidth, kDefaultBarWidth));
            line = build_line(label, include_postfix, bar_width, progress, elapsed_seconds, rate);
        }
        if (line.size() > terminal_width) {
            const std::string empty_label_line = build_line("", include_postfix, bar_width, progress, elapsed_seconds, rate);
            const size_t fixed_width =
                empty_label_line.size();
            const size_t label_width = terminal_width > fixed_width ? terminal_width - fixed_width : 0;
            label = ellipsize(label_, label_width);
            line = build_line(label, include_postfix, bar_width, progress, elapsed_seconds, rate);
        }
        if (line.size() > terminal_width && include_postfix) {
            include_postfix = false;
            line = build_line(label, include_postfix, bar_width, progress, elapsed_seconds, rate);
        }
        if (line.size() > terminal_width) {
            line = ellipsize(line, terminal_width);
        }

        std::fprintf(stderr, "\r\033[2K%s", line.c_str());
        std::fflush(stderr);
    }

    static std::string format_duration(double seconds) {
        const long long total_seconds = std::max<long long>(0, static_cast<long long>(std::llround(seconds)));
        const long long hours = total_seconds / 3600;
        const long long minutes = (total_seconds % 3600) / 60;
        const long long secs = total_seconds % 60;

        std::ostringstream stream;
        if (hours > 0) {
            stream << hours << "h " << minutes << "m " << secs << "s";
        } else if (minutes > 0) {
            stream << minutes << "m " << secs << "s";
        } else {
            stream << secs << "s";
        }
        return stream.str();
    }

    static std::string format_rate(double rate, const std::string& unit) {
        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        if (rate >= 100.0) {
            stream.precision(0);
        } else if (rate >= 10.0) {
            stream.precision(1);
        } else {
            stream.precision(2);
        }
        const std::string_view rendered_unit = unit == "batch" ? std::string_view("b") : std::string_view(unit);
        stream << rate << " " << rendered_unit << "/s";
        return stream.str();
    }

    static std::string ellipsize(const std::string& value, size_t max_width) {
        if (max_width == 0) {
            return {};
        }
        if (value.size() <= max_width) {
            return value;
        }
        if (max_width <= 3) {
            return value.substr(0, max_width);
        }
        return value.substr(0, max_width - 3) + "...";
    }

    static size_t detect_terminal_width() {
        struct winsize window_size {};
        if (::isatty(STDERR_FILENO) != 0 &&
            ::ioctl(STDERR_FILENO, TIOCGWINSZ, &window_size) == 0 &&
            window_size.ws_col >= 40) {
            return static_cast<size_t>(window_size.ws_col);
        }

        if (const char* columns = std::getenv("COLUMNS")) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(columns, &end, 10);
            if (end != columns && value >= 40) {
                return static_cast<size_t>(value);
            }
        }

        return 80;
    }

    std::string build_line(const std::string& label,
                           bool include_postfix,
                           int bar_width,
                           double progress,
                           double elapsed_seconds,
                           double rate) const {
        const int filled = total_ > 0 ? static_cast<int>(progress * static_cast<double>(bar_width)) : 0;

        std::ostringstream line;
        line << label << ": ";
        if (total_ > 0) {
            line << std::lround(progress * 100.0) << "%|";
        } else {
            line << "|";
        }
        for (int i = 0; i < bar_width; ++i) {
            line << (i < filled ? '#' : '-');
        }
        line << "| " << current_;
        if (total_ > 0) {
            line << "/" << total_;
        }
        line << " [" << format_duration(elapsed_seconds);
        if (total_ > 0 && rate > 0.0 && current_ < total_) {
            const double eta_seconds = static_cast<double>(total_ - current_) / rate;
            line << "<" << format_duration(eta_seconds);
        }
        line << ", " << format_rate(rate, unit_);
        if (include_postfix && !postfix_.empty()) {
            line << ", " << postfix_;
        }
        line << "]";
        return line.str();
    }

    std::string label_;
    size_t total_ = 0;
    std::string unit_;
    size_t current_ = 0;
    std::string postfix_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_render_;
    bool closed_ = false;
};

} // namespace fastloader::rfdetr
