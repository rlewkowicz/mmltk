#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include <unistd.h>

namespace mmltk::gui::console_output {

inline void trim_output_tail(std::string& output_tail, std::size_t max_size = 65536) {
    if (output_tail.size() > max_size) {
        output_tail.erase(0, output_tail.size() - max_size);
    }
}

inline void append_console_output(std::string& tail, const std::string_view chunk, std::size_t max_size = 65536) {
    std::size_t index = 0;
    while (index < chunk.size()) {
        const char ch = chunk[index];
        if (ch == '\r') {
            const std::size_t newline = tail.rfind('\n');
            if (newline == std::string::npos) {
                tail.clear();
            } else {
                tail.erase(newline + 1);
            }
            ++index;
            continue;
        }
        if (ch == '\b') {
            if (!tail.empty()) {
                tail.pop_back();
            }
            ++index;
            continue;
        }
        if (ch == '\033') {
            ++index;
            if (index < chunk.size() && chunk[index] == '[') {
                ++index;
                while (index < chunk.size()) {
                    const auto code = static_cast<unsigned char>(chunk[index++]);
                    if (code >= 0x40 && code <= 0x7E) {
                        break;
                    }
                }
            }
            continue;
        }
        tail.push_back(ch);
        ++index;
    }
    trim_output_tail(tail, max_size);
}

inline std::string read_fd(const int fd, const std::string_view error_prefix, const bool nonblocking = false) {
    std::string output;
    if (fd < 0) {
        return output;
    }

    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        throw std::runtime_error(std::string(error_prefix) + std::strerror(errno));
    }
    return output;
}

}  
