#include <csignal>

#include "catch2_compat.hpp"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/services/application_lifecycle_event.h"
#include "src/controller/services/firefox_process_owner.h"

namespace {

MMLTK_TEST_CASE("[browser_runtime][direct][presentation]", test_direct_presentation_descriptor) {
    const mmltk::controller::PresentationPublication publication{
        .capability =
            {
                .surface_high = 4U,
                .surface_low = 5U,
                .extent = {1'200U, 800U},
                .generation = 2U,
                .condition = mmltk::controller::PresentationCapabilityCondition::Ready,
            },
        .timeline_ready = 3U,
        .presentation_revision = 4U,
    };
    CHECK(publication.valid());
}

MMLTK_TEST_CASE("[browser_runtime][direct][shutdown]", test_direct_shutdown_result) {
    using namespace mmltk::controller::services;
    const FirefoxProcessLifecycle normal{
        .terminal = FirefoxProcessTerminal::Exited,
        .status = 0,
    };
    CHECK(browser_runtime_exit_status(normal, true) == 0);

    const FirefoxProcessLifecycle requested{
        .terminal = FirefoxProcessTerminal::Signaled,
        .status = 128 + SIGTERM,
        .stop_requested = true,
    };
    CHECK(browser_runtime_exit_status(requested, true) == 0);
    CHECK(ApplicationShutdownReason::SignalInterrupt != ApplicationShutdownReason::SignalTerminate);
}

}  // namespace
