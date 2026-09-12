#pragma once

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/subsystems/annotation/annotation_system.h"

namespace mmltk::testsupport {

template <class Events>
void await_annotation_command(controller::AnnotationSystem& annotation, Events& events, std::uint64_t admitted_revision) {
    REQUIRE(events.Wait([&] {
        const auto state = annotation.snapshot();
        return !state.busy && state.revision > admitted_revision;
    }));
}

template <class Events>
void await_annotation_render(controller::AnnotationSystem& annotation, Events& events) {
    REQUIRE(events.Wait([&] {
        const auto state = annotation.snapshot();
        return state.rendered.scene_revision == state.ui.scene_revision;
    }));
}

template <class Events>
void open_annotation(controller::AnnotationSystem& annotation, Events& events, const controller::VisualFrame& source) {
    static_cast<void>(annotation.Open({.source = source}));
    REQUIRE(events.Wait([&] {
        const auto state = annotation.snapshot();
        return state.ready && state.frame.valid();
    }));
}

}  // namespace mmltk::testsupport
