#include "gui/annotation/document/document.h"
#include "gui/annotation/session.h"
#include "gui/annotation/workflow_live_overlay.h"
#include "gui/cuda_gl_interop_error.h"
#include "gui/preview_interaction_overlay_types.h"
#include "mmltk/live/manual_overlay_document.h"
#include "overlay_compare_utils.h"
#include "live/live_helpers.h"
#include "cuda_test_utils.h"

#include "support/catch2_compat.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace mmltk::gui;
using namespace mmltk::live;

bool has_cuda_device() {
    int device_count = 0;
    return cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}

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

struct HostCallbackGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool open = false;
};

void wait_for_host_callback_gate(void* user_data) {
    auto* gate = static_cast<HostCallbackGate*>(user_data);
    std::unique_lock lock(gate->mutex);
    gate->condition.wait(lock, [&gate]() { return gate->open; });
}

void open_host_callback_gate(HostCallbackGate& gate) {
    {
        std::lock_guard lock(gate.mutex);
        gate.open = true;
    }
    gate.condition.notify_all();
}

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
    manual_b = manual_a;
    manual_b.brush_preview = ManualOverlayBrushPreview{42, 44, 12, true};
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
        cuda_gl_interop_error_message(cudaErrorInvalidGraphicsContext, "cudaGraphicsGLRegisterBuffer");
    assert(error_message.find("cudaGraphicsGLRegisterBuffer failed:") != std::string::npos);
    assert(error_message.find("valid hardware OpenGL or EGL context") != std::string::npos);
    assert(is_invalid_graphics_context_error(error_message));
    assert(!is_invalid_graphics_context_error("some unrelated error"));
}

void test_live_slot_helpers_prefer_latest_published_slot_and_fall_back() {
    auto slots = make_slots(3U);
    std::atomic<int> latest_index{2};
    slots[1]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    slots[2]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);

    int acquired_value = 0;
    const bool acquired_latest = try_acquire_latest_published_slot(slots, latest_index, &acquired_value,
                                                                   [](TestSlot& slot) { return slot.value; });
    assert(acquired_latest);
    assert(acquired_value == 3);
    assert(slots[2]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kAcquired));

    release_acquired_slot(*slots[2], "test latest slot release");
    slots[2]->state.store(to_slot_state_value(SlotState::kWriting), std::memory_order_release);
    acquired_value = 0;

    const bool acquired_fallback = try_acquire_latest_published_slot(slots, latest_index, &acquired_value,
                                                                     [](TestSlot& slot) { return slot.value; });
    assert(acquired_fallback);
    assert(acquired_value == 2);
    assert(slots[1]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kAcquired));
}

void test_live_slot_helpers_reuse_ready_stale_published_slot_only() {
    auto latest_only_slots = make_slots(2U);
    std::atomic<int> latest_only_index{1};
    latest_only_slots[0]->state.store(to_slot_state_value(SlotState::kAcquired), std::memory_order_release);
    latest_only_slots[1]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    latest_only_slots[1]->generation = 7U;

    int reset_count = 0;
    TestSlot* latest_slot = reserve_writable_slot(
        latest_only_slots, latest_only_index,
        [&reset_count](TestSlot& current) {
            ++reset_count;
            current.generation = 0U;
        },
        [](const TestSlot& current) { return current.ready_event; }, "test latest published slot reuse");

    assert(latest_slot == nullptr);
    assert(reset_count == 0);
    assert(latest_only_slots[1]->generation == 7U);
    assert(latest_only_index.load(std::memory_order_acquire) == 1);
    assert(latest_only_slots[1]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kPublished));

    auto slots = make_slots(3U);
    std::atomic<int> latest_index{2};
    slots[0]->state.store(to_slot_state_value(SlotState::kAcquired), std::memory_order_release);
    slots[1]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    slots[2]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);
    slots[1]->generation = 9U;
    slots[2]->generation = 10U;

    TestSlot* slot = reserve_writable_slot(
        slots, latest_index,
        [&reset_count](TestSlot& current) {
            ++reset_count;
            current.generation = 0U;
        },
        [](const TestSlot& current) { return current.ready_event; }, "test published slot reuse");

    assert(slot != nullptr);
    assert(slot->slot_index == 1U);
    assert(reset_count == 1);
    assert(slot->generation == 0U);
    assert(latest_index.load(std::memory_order_acquire) == 2);
    assert(slots[2]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kPublished));
    assert(slot->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kWriting));
}

void test_live_slot_helpers_gate_acquire_on_ready_event_completion() {
    if (!has_cuda_device()) {
        SKIP("no CUDA device available");
    }

    ensure_cuda_ok(cudaSetDevice(0), "cudaSetDevice for ready-gated live slot test");
    CudaEventHandle ready_event;
    CudaStreamHandle stream;
    ready_event.create(cudaEventDisableTiming, "cudaEventCreate ready for ready-gated live slot test");
    stream.create_with_highest_priority("cudaStreamCreate for ready-gated live slot test");

    HostCallbackGate gate;
    ensure_cuda_ok(cudaLaunchHostFunc(stream.get(), wait_for_host_callback_gate, &gate),
                   "cudaLaunchHostFunc for ready-gated live slot test");
    ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()),
                   "cudaEventRecord ready for ready-gated live slot test");

    auto slots = make_slots(1U);
    std::atomic<int> latest_index{0};
    slots[0]->ready_event = ready_event.get();
    slots[0]->state.store(to_slot_state_value(SlotState::kPublished), std::memory_order_release);

    int acquired_value = 0;
    const bool acquired_before_ready = try_acquire_latest_ready_published_slot(
        slots, latest_index, &acquired_value, [](TestSlot& slot) { return slot.value; },
        [](TestSlot& slot) { return slot.ready_event; }, "cudaEventQuery for ready-gated live slot test");
    assert(!acquired_before_ready);
    assert(acquired_value == 0);
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kPublished));

    open_host_callback_gate(gate);
    ensure_cuda_ok(cudaEventSynchronize(ready_event.get()),
                   "cudaEventSynchronize ready for ready-gated live slot test");

    const bool acquired_after_ready = try_acquire_latest_ready_published_slot(
        slots, latest_index, &acquired_value, [](TestSlot& slot) { return slot.value; },
        [](TestSlot& slot) { return slot.ready_event; }, "cudaEventQuery for ready-gated live slot test");
    assert(acquired_after_ready);
    assert(acquired_value == 1);
    assert(slots[0]->state.load(std::memory_order_acquire) == to_slot_state_value(SlotState::kAcquired));
    release_acquired_slot(*slots[0], "ready-gated live slot release");
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

    const auto request = make_annotation_workflow_live_overlay_request(&document, &session, &frame);
    assert(annotation_workflow_live_overlay_needs_publish(runtime.live_overlay, request));

    note_annotation_workflow_live_overlay_published(runtime, document, session, frame);
    assert(!annotation_workflow_live_overlay_needs_publish(runtime.live_overlay, request));

    session.select_object(3U);
    assert(annotation_workflow_live_overlay_needs_publish(runtime.live_overlay, request));
    note_annotation_workflow_live_overlay_published(runtime, document, session, frame);

    frame.capture_width = 800U;
    assert(annotation_workflow_live_overlay_needs_publish(
        runtime.live_overlay, make_annotation_workflow_live_overlay_request(&document, &session, &frame)));
}

void test_live_pitched_buffers_fit_bgr_and_rgba_pixel_rows() {
    if (!has_cuda_device()) {
        SKIP("no CUDA device available");
    }

    constexpr std::uint32_t width = 17U;
    constexpr std::uint32_t height = 9U;
    const std::size_t bgr_row_bytes = static_cast<std::size_t>(width) * sizeof(Bgr24Pixel);
    const std::size_t rgba_row_bytes = static_cast<std::size_t>(width) * sizeof(Rgba32Pixel);

    CUDA_ASSERT_OK(cudaSetDevice(0));
    cudaStream_t stream = nullptr;
    CUDA_ASSERT_OK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    BgrPitchedDeviceBuffer bgr_buffer;
    bgr_buffer.ensure_dimensions(width, height, "cudaMallocPitch for BGR pitched live buffer test");
    assert(bgr_buffer.width() == width);
    assert(bgr_buffer.height() == height);
    assert(bgr_buffer.pitch_bytes() >= bgr_row_bytes);

    std::vector<std::uint8_t> bgr_input(bgr_row_bytes * static_cast<std::size_t>(height), 0x5AU);
    CUDA_ASSERT_OK(cudaMemcpy2DAsync(device_ptr_as_void(bgr_buffer.data()), bgr_buffer.pitch_bytes(), bgr_input.data(),
                                     bgr_row_bytes, bgr_row_bytes, height, cudaMemcpyHostToDevice, stream));

    RgbaPitchedDeviceBuffer rgba_buffer;
    rgba_buffer.ensure_dimensions(width, height, "cudaMallocPitch for RGBA pitched live buffer test");
    assert(rgba_buffer.width() == width);
    assert(rgba_buffer.height() == height);
    assert(rgba_buffer.pitch_bytes() >= rgba_row_bytes);
    CUDA_ASSERT_OK(cudaMemset2DAsync(device_ptr_as_void(rgba_buffer.data()), rgba_buffer.pitch_bytes(), 0,
                                     rgba_row_bytes, height, stream));

    CUDA_ASSERT_OK(cudaStreamSynchronize(stream));
    CUDA_ASSERT_OK(cudaStreamDestroy(stream));
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_overlay_compare_helpers_detect_boundary_changes);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_manual_overlay_document_suppresses_identical_replays);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]",
                         test_gui_interop_helpers_report_invalid_graphics_context_errors);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]",
                         test_live_slot_helpers_prefer_latest_published_slot_and_fall_back);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]", test_live_slot_helpers_reuse_ready_stale_published_slot_only);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime][cuda]",
                         test_live_slot_helpers_gate_acquire_on_ready_event_completion);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime]",
                         test_annotation_workflow_live_overlay_state_tracks_boundary_changes);
MMLTK_REGISTER_TEST_CASE("[gui][live_overlay_runtime][cuda]", test_live_pitched_buffers_fit_bgr_and_rgba_pixel_rows);
