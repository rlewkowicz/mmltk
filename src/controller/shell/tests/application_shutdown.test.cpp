#include <csignal>

#include <catch2/catch_test_macros.hpp>

#include "src/controller/services/firefox_process_owner.h"

namespace mmltk::controller::shell {
namespace {

TEST_CASE("desktop exit status follows ordinary Firefox and shutdown results") {
    const services::FirefoxProcessLifecycle exited{.terminal = services::FirefoxProcessTerminal::Exited, .status = 0};
    CHECK(services::browser_runtime_exit_status(exited, true) == 0);
    CHECK(services::browser_runtime_exit_status(exited, false) == 1);

    const services::FirefoxProcessLifecycle stopped{
        .terminal = services::FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM, .stop_requested = true};
    CHECK(services::browser_runtime_exit_status(stopped, true) == 0);
}

}  // namespace
}  // namespace mmltk::controller::shell
