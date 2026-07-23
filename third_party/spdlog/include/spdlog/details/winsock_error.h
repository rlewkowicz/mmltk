
#pragma once

#include <spdlog/common.h>
#include <spdlog/details/windows_include.h>

#include <string>
#include <winsock2.h>

namespace spdlog {
namespace details {

inline void throw_winsock_error(const char* sink_name, const std::string& msg, int last_error) {
    char buf[512];
    ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_error,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, (sizeof(buf) / sizeof(char)), NULL);
    throw_spdlog_ex(fmt_lib::format("{} - {}: {}", sink_name, msg, buf));
}

}  
}  
