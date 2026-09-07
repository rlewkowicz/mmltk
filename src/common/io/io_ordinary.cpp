#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "src/common/io/event_fd.h"
#include "src/common/io/file_memory.h"
#include "src/common/io/filesystem_utils.h"
#include "src/common/io/noexcept_io.h"
#include "src/common/io/scoped_fd.h"
#include "src/common/io/staging_directory.h"

namespace mmltk::common::io {

bool signal_event_fd(const int descriptor) noexcept {
    if (descriptor < 0) return false;
    constexpr std::uint64_t value = 1U;
    ssize_t written = -1;
    do {
        written = ::write(descriptor, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(sizeof(value)) || (written < 0 && errno == EAGAIN);
}

EventFdWait wait_event_fd(const int descriptor) noexcept {
    pollfd readiness{.fd = descriptor, .events = POLLIN, .revents = 0};
    int poll_result = -1;
    do {
        poll_result = ::poll(&readiness, 1U, -1);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result != 1 || (readiness.revents & POLLIN) == 0) return {EventFdWaitStatus::WaitFailed, 0U};
    std::uint64_t count = 0U;
    ssize_t result = -1;
    do {
        result = ::read(descriptor, &count, sizeof(count));
    } while (result < 0 && errno == EINTR);
    return result == static_cast<ssize_t>(sizeof(count)) ? EventFdWait{EventFdWaitStatus::Woken, count}
                                                         : EventFdWait{EventFdWaitStatus::ReadFailed, 0U};
}

void drain_event_fd(const int descriptor) noexcept {
    if (descriptor < 0) return;
    std::uint64_t value = 0U;
    for (;;) {
        const ssize_t count = ::read(descriptor, &value, sizeof(value));
        if (count == static_cast<ssize_t>(sizeof(value))) continue;
        if (count < 0 && errno == EINTR) continue;
        return;
    }
}

namespace filesystem_utils {
void remove_path_recursively_best_effort(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) return;
    const std::string native = path.string();
    struct stat status{};
    if (::lstat(native.c_str(), &status) != 0) return;
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        static_cast<void>(::unlink(native.c_str()));
        return;
    }
    DIR* const directory = ::opendir(native.c_str());
    if (directory == nullptr) {
        static_cast<void>(::rmdir(native.c_str()));
        return;
    }
    while (dirent* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            remove_path_recursively_best_effort(path / entry->d_name);
    }
    static_cast<void>(::closedir(directory));
    static_cast<void>(::rmdir(native.c_str()));
}
}  // namespace filesystem_utils

bool try_write_all_noexcept(const int descriptor, std::string_view data) noexcept {
    while (!data.empty()) {
        const ssize_t written = ::write(descriptor, data.data(), data.size());
        if (written > 0) {
            data.remove_prefix(static_cast<std::size_t>(written));
        } else if (written >= 0 || errno != EINTR) {
            return false;
        }
    }
    return true;
}

void write_all_noexcept(const int descriptor, const std::string_view data) noexcept {
    static_cast<void>(try_write_all_noexcept(descriptor, data));
}

ScopedFd::ScopedFd(const int descriptor) noexcept : fd_(descriptor) {}
ScopedFd::~ScopedFd() { reset(); }
ScopedFd::ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept {
    if (this != &other) reset(std::exchange(other.fd_, -1));
    return *this;
}
int ScopedFd::get() const noexcept { return fd_; }
int ScopedFd::release() noexcept { return std::exchange(fd_, -1); }
void ScopedFd::reset(const int next) noexcept {
    if (fd_ >= 0) static_cast<void>(::close(fd_));
    fd_ = next;
}

StagingDirectory::StagingDirectory(const std::filesystem::path& destination, const std::string_view prefix, const std::string_view suffix,
                                   const char* const failure_action) {
    const std::filesystem::path parent = destination.parent_path().empty() ? std::filesystem::path{"."} : destination.parent_path();
    std::filesystem::create_directories(parent);
    const std::string pattern = (parent / (std::string(prefix) + destination.filename().string() + std::string(suffix))).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (::mkdtemp(writable.data()) == nullptr) throw errno_error(failure_action, pattern);
    path_ = writable.data();
}
StagingDirectory::~StagingDirectory() {
    if (!published_) {
        std::error_code ignored;
        static_cast<void>(remove_tree_no_follow(path_, ignored));
    }
}
const std::filesystem::path& StagingDirectory::path() const noexcept { return path_; }
void StagingDirectory::published() noexcept { published_ = true; }

}  // namespace mmltk::common::io
