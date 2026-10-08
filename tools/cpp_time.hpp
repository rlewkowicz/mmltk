#pragma once

#include "cpp_support.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace mmltk::tools {

enum class TimestampFormat { javascript, python };
inline std::string utc_timestamp(TimestampFormat format, std::string_view failure) {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto epoch = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (!::gmtime_r(&epoch, &utc) && !failure.empty()) throw ToolError(std::string(failure));
    const bool milliseconds = format == TimestampFormat::javascript;
    const auto fraction = milliseconds
        ? std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count()
        : std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count();
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (milliseconds || fraction) stream << '.' << std::setfill('0') << std::setw(milliseconds ? 3 : 6) << fraction;
    stream << (milliseconds ? "Z" : "+00:00");
    return stream.str();
}

} // namespace mmltk::tools
