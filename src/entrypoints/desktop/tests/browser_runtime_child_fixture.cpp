#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <string_view>
#include "browser_runtime_fixture_args.hpp"
namespace {
[[nodiscard]] int publish_readiness(const int descriptor) noexcept {
    const std::uint64_t fact = 1U;
    return ::write(descriptor, &fact, sizeof(fact)) == static_cast<ssize_t>(sizeof(fact)) ? 0 : 67;
}
[[nodiscard]] int wait_for_term(const int readiness_descriptor) noexcept {
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0) { return 65; }
    // The production child is deliberately spawned with TERM unblocked.  These
    // controlled sigwaitinfo modes establish their own waiting mask.
    if (::sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) return 66;
    if (readiness_descriptor >= 0 && publish_readiness(readiness_descriptor) != 0) return 67;
    const int signal = ::sigwaitinfo(&signals, nullptr);
    return signal == SIGTERM ? 0 : 68;
}
volatile sig_atomic_t term_handled = 0;
void handle_term(const int) noexcept { term_handled = 1; }
[[nodiscard]] int handle_term_normally(const int readiness_descriptor) noexcept {
    struct sigaction action{};
    action.sa_handler = &handle_term;
    if (::sigemptyset(&action.sa_mask) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) { return 72; }
    if (publish_readiness(readiness_descriptor) != 0) return 73;
    while (term_handled == 0) {
        if (::pause() == -1 && errno != EINTR) return 74;
    }
    return 0;
}
[[nodiscard]] int refuse_term_until_kill(const int readiness_descriptor) noexcept {
    if (wait_for_term(readiness_descriptor) != 0) return 69;
    sigset_t unblocked{};
    if (::sigemptyset(&unblocked) != 0) return 70;
    // SIGKILL cannot be caught or waited for.  sigsuspend therefore blocks
    // without a timer until the process owner selects its one KILL escalation.
    return ::sigsuspend(&unblocked) == -1 ? 71 : 72;
}
[[nodiscard]] int parse_readiness_descriptor(const std::string_view argument, std::string_view* const mode) noexcept {
    const std::size_t separator = argument.find(':');
    if (separator == std::string_view::npos || mode == nullptr || separator == 0U || separator + 1U == argument.size()) { return -1; }
    *mode = argument.substr(0U, separator);
    int descriptor = 0;
    for (const char character : argument.substr(separator + 1U)) {
        if (character < '0' || character > '9' || descriptor > 214748364 || (descriptor == 214748364 && character > '7')) { return -1; }
        descriptor = descriptor * 10 + (character - '0');
    }
    return descriptor;
}
}  // namespace
int main(const int argc, char* argv[]) {
    if (!valid_browser_runtime_fixture_args(argc, argv)) { return 64; }
    const std::string_view argument{argv[9]};
    if (argument == "normal-exit") return 0;
    if (argument == "failure-exit") return 75;
    if (argument == "term-ack") return wait_for_term(-1);
    if (argument == "term-refuse") return refuse_term_until_kill(-1);
    std::string_view mode;
    const int readiness_descriptor = parse_readiness_descriptor(argument, &mode);
    if (readiness_descriptor < 0) return 64;
    if (mode == "term-ready-fail") return 75;
    if (mode == "term-ready-fd-error") {
        if (::close(readiness_descriptor) != 0) return 76;
        return publish_readiness(readiness_descriptor);
    }
    if (mode == "term-ack") return wait_for_term(readiness_descriptor);
    if (mode == "term-handler") return handle_term_normally(readiness_descriptor);
    if (mode == "term-refuse") return refuse_term_until_kill(readiness_descriptor);
    return 69;
}
