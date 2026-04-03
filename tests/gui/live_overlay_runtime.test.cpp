#include "gui/annotation/document/document.h"
#include "gui/annotation/session.h"
#include "gui/annotation/workflow_live.h"
#include "gui/cuda_gl_interop_utils.h"
#include "mmltk/live/manual_overlay_document.h"
#include "overlay_compare_utils.h"
#include "live/live_helpers.h"

#include "support/catch2_compat.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace mmltk::gui;
using namespace mmltk::live;

ManualOverlayDocumentSnapshot make_manual_snapshot() {
    ManualOverlayDocumentSnapshot snapshot;
    snapshot.capture_width = 640U;
    snapshot.capture_height = 360U;
    snapshot.document_generation = 3U;
    snapshot.session_revision = 5U;
    snapshot.selected_instance = 0U;

    ManualOverlayInstance instance;
    instance.instance_id = "manual-0";
    instance.category_index = 2U;
    instance.box = ManualOverlayBox{10, 20, 110, 160};
    instance.mask_region = ManualOverlayMaskRegion{4U, 8U, 3U, 2U};
    instance.mask = {1U, 0U, 1U, 0U, 1U, 0U};
    instance.polyline_points = {
        ManualOverlayPoint{11, 21},
        ManualOverlayPoint{44, 55},
    };
    instance.polyline_closed = true;
    instance.points = {
        ManualOverlayPoint{30, 40},
        ManualOverlayPoint{80, 90},
    };
    instance.skeleton_edges = {
        ManualOverlayEdge{0U, 1U},
    };
    snapshot.instances.push_back(std::move(instance));
    return snapshot;
}

PreviewInteractionOverlaySnapshot make_preview_snapshot() {
    PreviewInteractionOverlaySnapshot snapshot;
    snapshot.width = 640U;
    snapshot.height = 360U;
    snapshot.cuda_device_index = 2;
    snapshot.boxes = {
        PreviewInteractionOverlayBox{AnnotationBox{1, 2, 30, 40}, 255U, 220U, 96U, 3, true, 7},
    };
    snapshot.polylines = {
        PreviewInteractionOverlayPolyline{
            {
                PreviewInteractionOverlayPoint{5, 6},
                PreviewInteractionOverlayPoint{7, 8},
            },
            true,
            240U,
            196U,
            68U,
            2,
        },
    };
    snapshot.marker_sets = {
        PreviewInteractionOverlayMarkerSet{
            {
                PreviewInteractionOverlayPoint{10, 11},
            },
            4,
            255U,
            255U,
            255U,
            200U,
        },
    };
    snapshot.skeletons = {
        PreviewInteractionOverlaySkeleton{
            {
                PreviewInteractionOverlayPoint{12, 13},
                PreviewInteractionOverlayPoint{14, 15},
            },
            {
                PreviewInteractionOverlayEdge{0U, 1U},
            },
            124U,
            198U,
            255U,
            2,
        },
    };
    return snapshot;
}

struct TestSlot {
    std::uint32_t slot_index = 0;
    std::atomic<std::uint32_t> state{to_slot_state_value(SlotState::kFree)};
    std::uint64_t generation = 0;
    cudaEvent_t ready_event = nullptr;
    int value = 0;
};

std::vector<std::unique_ptr<TestSlot>> make_slots(std::size_t count) {
    std::vector<std::unique_ptr<TestSlot>> slots;
    slots.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto slot = std::make_unique<TestSlot>();
        slot->slot_index = static_cast<std::uint32_t>(index);
        slot->value = static_cast<int>(index + 1);
        slots.push_back(std::move(slot));
    }
    return slots;
}

void test_overlay_compare_helpers_detect_boundary_changes() {
    const ManualOverlayDocumentSnapshot manual_a = make_manual_snapshot();
    ManualOverlayDocumentSnapshot manual_b = manual_a;
    assert(mmltk::overlay_compare::manual_snapshot_equals(manual_a, manual_b));
    manual_b.instances[0].points.push_back(ManualOverlayPoint{99, 100});
    assert(!mmltk::overlay_compare::manual_snapshot_equals(manual_a, manual_b));

    const PreviewInteractionOverlaySnapshot preview_a = make_preview_snapshot();
    PreviewInteractionOverlaySnapshot preview_b = preview_a;
    assert(mmltk::overlay_compare::preview_snapshot_equals(preview_a, preview_b));
    preview_b.boxes[0].handle_radius = 9;
    assert(!mmltk::overlay_compare::preview_snapshot_equals(preview_a, preview_b));
}

void test_manual_overlay_document_suppresses_identical_replays() {
    ManualOverlayDocument document;
    ManualOverlayDocumentSnapshot snapshot = make_manual_snapshot();

    document.publish_snapshot(snapshot);
    const auto first = document.snapshot();
    assert(first);
    assert(first->generation == 1U);

    document.publish_snapshot(snapshot);
    const auto second = document.snapshot();
    assert(second);
    assert(second->generation == 1U);
    assert(second->instances.size() == 1U);

    snapshot.instances[0].enabled = false;
    document.publish_snapshot(std::move(snapshot));
    const auto third = document.snapshot();
    assert(third);
    assert(third->generation == 2U);
    assert(!third->instances[0].enabled);
}

void test_gui_interop_helpers_report_invalid_graphics_context_errors() {
    const std::string error_message =
        cuda_gl_interop_error_message(cudaErrorInvalidGraphicsContext,
                                      "cudaGraphicsGLRegisterBuffer");
    assert(error_message.find("cudaGraphicsGLRegisterBuffer failed:") != std::string::npos);
    assert(error_message.find("valid hardware OpenGL context") != std::string::npos);
    assert(is_invalid_graphics_context_error(error_message));
    assert(!is_invalid_graphics_context_error("some unrelated error"));
}

void test_live_slot_helpers_prefer_latest_published_slot_and_fall_back() {
    auto slots = make_slots(3U);
    std::atomic<int> latest_index{2};
    slots[1]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    slots[2]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);

    int acquired_value = 0;
    const bool acquired_latest =
        try_acquire_latest_published_slot(slots,
                                          latest_index,
                                          &acquired_value,
                                          [](TestSlot& slot) { return slot.value; });
    assert(acquired_latest);
    assert(acquired_value == 3);
    assert(slots[2]->state.load(std::memory_order_acquire) ==
           to_slot_state_value(SlotState::kAcquired));

    release_acquired_slot(*slots[2], "test latest slot release");
    slots[2]->state.store(to_slot_state_value(SlotState::kWriting), std::memory_order_release);
    acquired_value = 0;

    const bool acquired_fallback =
        try_acquire_latest_published_slot(slots,
                                          latest_index,
                                          &acquired_value,
                                          [](TestSlot& slot) { return slot.value; });
    assert(acquired_fallback);
    assert(acquired_value == 2);
    assert(slots[1]->state.load(std::memory_order_acquire) ==
           to_slot_state_value(SlotState::kAcquired));
}

void test_live_slot_helpers_reuse_ready_published_slot_and_reset_latest_index() {
    auto slots = make_slots(2U);
    std::atomic<int> latest_index{1};
    slots[0]->state.store(to_slot_state_value(SlotState::kAcquired), std::memory_order_release);
    slots[1]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    slots[1]->generation = 9U;

    int reset_count = 0;
    TestSlot* slot = reserve_writable_slot(
        slots,
        latest_index,
        [&reset_count](TestSlot& current) {
            ++reset_count;
            current.generation = 0U;
        },
        [](const TestSlot& current) { return current.ready_event; },
        "test published slot reuse");

    assert(slot != nullptr);
    assert(slot->slot_index == 1U);
    assert(reset_count == 1);
    assert(slot->generation == 0U);
    assert(latest_index.load(std::memory_order_acquire) == -1);
    assert(slot->state.load(std::memory_order_acquire) ==
           to_slot_state_value(SlotState::kWriting));
}

void test_annotation_workflow_live_overlay_state_tracks_boundary_changes() {
    AnnotationWorkflowRuntime runtime;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationFrame frame;
    frame.width = 320U;
    frame.height = 180U;
    frame.capture_width = 640U;
    frame.capture_height = 360U;

    const auto request =
        make_annotation_workflow_live_overlay_request(&document, &session, &frame);
    assert(annotation_workflow_live_overlay_needs_publish(runtime.live_overlay, request));

    note_annotation_workflow_live_overlay_published(runtime, document, session, frame);
    assert(!annotation_workflow_live_overlay_needs_publish(runtime.live_overlay, request));

    session.select_object(3U);
    assert(annotation_workflow_live_overlay_needs_publish(runtime.live_overlay, request));
    note_annotation_workflow_live_overlay_published(runtime, document, session, frame);

    frame.capture_width = 800U;
    assert(annotation_workflow_live_overlay_needs_publish(
        runtime.live_overlay,
        make_annotation_workflow_live_overlay_request(&document, &session, &frame)));
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_overlay_compare_helpers_detect_boundary_changes);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_manual_overlay_document_suppresses_identical_replays);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_gui_interop_helpers_report_invalid_graphics_context_errors);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_live_slot_helpers_prefer_latest_published_slot_and_fall_back);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_live_slot_helpers_reuse_ready_published_slot_and_reset_latest_index);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_annotation_workflow_live_overlay_state_tracks_boundary_changes);
