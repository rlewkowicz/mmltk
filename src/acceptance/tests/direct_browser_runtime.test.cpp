#include "catch2_compat.hpp"
#include "src/controller/presentation/presentation_system.h"
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
}  // namespace
