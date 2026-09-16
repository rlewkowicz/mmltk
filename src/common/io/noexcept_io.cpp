#include "src/common/io/noexcept_io.h"
#include <unistd.h>
#include <cerrno>
namespace mmltk::common::io {
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
void write_all_noexcept(const int descriptor, const std::string_view data) noexcept { static_cast<void>(try_write_all_noexcept(descriptor, data)); }
}  // namespace mmltk::common::io
