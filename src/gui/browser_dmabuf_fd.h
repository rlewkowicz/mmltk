#pragma once

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>

namespace mmltk::gui {

[[nodiscard]] inline int duplicate_workspace_dmabuf_fd(const int fd, const char* context) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) {
        throw std::runtime_error(std::string(context) + ": " + std::strerror(errno));
    }
    return duplicate;
}

}  // namespace mmltk::gui
