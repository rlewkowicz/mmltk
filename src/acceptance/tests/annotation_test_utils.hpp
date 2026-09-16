#pragma once
#include <cstdint>
#include <catch2/catch_test_macros.hpp>
#include "src/controller/presentation/visual_system_types.h"
#include "src/controller/presentation/visual_diagnostics.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
namespace mmltk::testsupport {
inline controller::VisualDiagnosticSink annotation_render_evidence() {
    static unsigned char enabled;
    return {.context = &enabled, .write = [](void*, controller::VisualDiagnosticFact) noexcept {}};
}
inline controller::WorkspaceMouse annotation_mouse(controller::AnnotationSystem& annotation, std::uint64_t peer, controller::WorkspaceMouseKind kind,
                                                   controller::WorkspacePoint point) {
    return {.source = controller::PresentationSourceKind::Annotation,
            .peer_epoch = peer,
            .document_epoch = annotation.snapshot().input_document_epoch,
            .kind = kind,
            .point = point,
            .brush_radius = controller::contracts::kDefaultAnnotationBrushRadius};
}
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
        const auto image = annotation.ImageSnapshot(state.frame);
        return image && image->diagnostics && image->diagnostics->scene_revision == state.ui.scene_revision;
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
