#include "gui/cef_signal_restore.h"

#include <csignal>

#include <array>
#include <cstddef>
#include <cstring>

namespace mmltk::gui {

namespace {

constexpr std::array<int, 13> kSignalsToRestore{
    SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGALRM, SIGTERM, SIGCHLD, SIGBUS, SIGTRAP, SIGPIPE,
};

std::array<struct sigaction, kSignalsToRestore.size()> g_signal_handlers{};

}  // namespace

void backup_posix_signal_handlers() noexcept {
    struct sigaction current;
    for (std::size_t i = 0; i < kSignalsToRestore.size(); ++i) {
        std::memset(&current, 0, sizeof(current));
        ::sigaction(kSignalsToRestore[i], nullptr, &current);
        g_signal_handlers[i] = current;
    }
}

void restore_posix_signal_handlers() noexcept {
    for (std::size_t i = 0; i < kSignalsToRestore.size(); ++i) {
        ::sigaction(kSignalsToRestore[i], &g_signal_handlers[i], nullptr);
    }
}

}  // namespace mmltk::gui
