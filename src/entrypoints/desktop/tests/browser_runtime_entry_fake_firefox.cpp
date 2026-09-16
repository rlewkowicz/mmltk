#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include "browser_runtime_fixture_args.hpp"
namespace {
constexpr std::string_view kPagePrefix{"http://127.0.0.1:"};
constexpr std::string_view kFixtureMarker{"mmltk browser runtime entry fixture"};
struct PageTarget final {
    std::uint16_t port = 0U;
    std::string_view path;
};
[[nodiscard]] bool parse_page(const std::string_view page, PageTarget* const target) noexcept {
    if (target == nullptr || !page.starts_with(kPagePrefix)) return false;
    const std::size_t port_end = page.find('/', kPagePrefix.size());
    if (port_end == std::string_view::npos || page.substr(port_end).find("?session=") == std::string_view::npos ||
        page.substr(port_end).find("&mmltk_ws_url=ws%3A%2F%2F127.0.0.1%3A") == std::string_view::npos) {
        return false;
    }
    unsigned int port = 0U;
    const std::string_view port_text = page.substr(kPagePrefix.size(), port_end - kPagePrefix.size());
    const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || port == 0U || port > std::numeric_limits<std::uint16_t>::max()) { return false; }
    target->port = static_cast<std::uint16_t>(port);
    target->path = page.substr(port_end);
    return true;
}
[[nodiscard]] bool send_request(const int descriptor, const PageTarget target) noexcept {
    const std::string request =
        "GET " + std::string{target.path} + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(target.port) + "\r\nConnection: close\r\n\r\n";
    std::size_t written = 0U;
    while (written != request.size()) {
        const ssize_t bytes = ::send(descriptor, request.data() + written, request.size() - written, MSG_NOSIGNAL);
        if (bytes <= 0) return false;
        written += static_cast<std::size_t>(bytes);
    }
    return true;
}
[[nodiscard]] int fetch_page(const PageTarget target) noexcept {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return 70;
    const timeval timeout{.tv_sec = 3, .tv_usec = 0};
    if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        static_cast<void>(::close(descriptor));
        return 71;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(target.port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 || ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        !send_request(descriptor, target)) {
        static_cast<void>(::close(descriptor));
        return 72;
    }
    std::array<char, 4096U> response{};
    std::size_t received = 0U;
    for (;;) {
        const ssize_t bytes = ::recv(descriptor, response.data() + received, response.size() - received, 0);
        if (bytes <= 0) break;
        received += static_cast<std::size_t>(bytes);
        const std::string_view page{response.data(), received};
        if (page.starts_with("HTTP/1.1 200") && page.find(kFixtureMarker) != std::string_view::npos) {
            static_cast<void>(::close(descriptor));
            return 0;
        }
        if (received == response.size()) break;
    }
    static_cast<void>(::close(descriptor));
    return received == 0U ? 73 : 74;
}
}  // namespace
int main(const int argc, char* argv[]) {
    if (!valid_browser_runtime_fixture_args(argc, argv)) { return 64; }
    PageTarget target;
    if (!parse_page(argv[9], &target)) return 65;
    const int fetch_result = fetch_page(target);
    if (fetch_result != 0) return fetch_result;
    const std::string_view page{argv[9]};
    const char* pixel = std::getenv("MMLTK_GUI_PIXEL_TRACE");
    if (std::printf("mmltk fake Firefox tracing lifecycle=%d pixels=%d environment=%d\n",
                    static_cast<int>(page.find("mmltk_surface_trace=1") != std::string_view::npos),
                    static_cast<int>(page.find("mmltk_pixel_trace=1") != std::string_view::npos),
                    static_cast<int>(pixel != nullptr && std::string_view{pixel} == "1")) < 0)
        return 75;
    if (std::fputs("mmltk fake Firefox stdout\n", stdout) == EOF || std::fputs("mmltk fake Firefox stderr\n", stderr) == EOF || std::fflush(stdout) != 0 ||
        std::fflush(stderr) != 0) {
        return 75;
    }
    return 0;
}
