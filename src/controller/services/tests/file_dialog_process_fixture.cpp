#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>

namespace {

[[nodiscard]] int entry_descriptor() noexcept {
    const char* const configured = std::getenv("MMLTK_FILE_DIALOG_ENTRY_FD");
    if (configured == nullptr) return -1;
    int descriptor = -1;
    const char* const end = configured + std::char_traits<char>::length(configured);
    const auto [parsed, error] = std::from_chars(configured, end, descriptor);
    return error == std::errc{} && parsed == end && descriptor >= 0 ? descriptor : -1;
}

[[nodiscard]] bool publish_entry(const int descriptor) noexcept {
    const std::uint64_t one = 1U;
    ssize_t written = -1;
    do {
        written = ::write(descriptor, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(sizeof(one));
}

}  // namespace

int main() {
    const int descriptor = entry_descriptor();
    if (descriptor < 0 || !publish_entry(descriptor)) return 2;
    while (::pause() < 0 && errno == EINTR) {}
    return 0;
}
