#include "gui/live_session_utils.h"

#include "mmltk/live/live_pipeline_trace.h"
#include "support/catch2_compat.hpp"

#include <stdexcept>
#include <string>

namespace {

using mmltk::gui::AnnotationBox;
using mmltk::gui::AnnotationFrame;
using mmltk::gui::AnnotateViewState;
using mmltk::gui::ModelInputMode;
using mmltk::gui::SourceKind;
using mmltk::gui::SourceSelectionState;
using mmltk::live::LiveFrameId;
using mmltk::live::UiCropState;

SourceSelectionState make_video_source() {
    SourceSelectionState source;
    source.kind = SourceKind::VideoStream;
    source.capture_width = 1920;
    source.capture_height = 1080;
    return source;
}

void test_seed_runtime_crop_from_source_uses_persisted_crop() {
    SourceSelectionState source = make_video_source();
    source.crop_x = 320;
    source.crop_y = 180;
    source.crop_width = 640;
    source.crop_height = 360;

    UiCropState ui_crop_state;
    mmltk::gui::seed_runtime_crop_from_source(ui_crop_state, source);

    const auto snapshot = ui_crop_state.snapshot();
    assert(snapshot);
    assert(snapshot->has_crop);
    assert(snapshot->region.x == 320U);
    assert(snapshot->region.y == 180U);
    assert(snapshot->region.width == 640U);
    assert(snapshot->region.height == 360U);
}

void test_seed_runtime_crop_from_source_clears_full_capture() {
    const SourceSelectionState source = make_video_source();

    UiCropState ui_crop_state;
    mmltk::gui::seed_runtime_crop_from_source(ui_crop_state, source);

    const auto snapshot = ui_crop_state.snapshot();
    assert(snapshot);
    assert(!snapshot->has_crop);
}

void test_runtime_crop_mirror_round_trips_partial_crop_and_clears_full_capture() {
    SourceSelectionState source = make_video_source();
    UiCropState ui_crop_state;

    mmltk::gui::publish_runtime_crop_box(ui_crop_state, source, AnnotationBox{40, 50, 400, 300});
    mmltk::gui::mirror_runtime_crop_to_source(ui_crop_state, &source);
    assert(source.crop_x == 40);
    assert(source.crop_y == 50);
    assert(source.crop_width == 360);
    assert(source.crop_height == 250);

    mmltk::gui::publish_runtime_crop_box(ui_crop_state, source, mmltk::gui::full_capture_box_for_source(source));
    mmltk::gui::mirror_runtime_crop_to_source(ui_crop_state, &source);
    assert(source.crop_x == 0);
    assert(source.crop_y == 0);
    assert(source.crop_width == 0);
    assert(source.crop_height == 0);
}

void test_persist_crop_box_to_source_round_trips_partial_crop_and_clamps_out_of_bounds() {
    SourceSelectionState source = make_video_source();

    mmltk::gui::persist_crop_box_to_source(AnnotationBox{40, 50, 400, 300}, &source);
    assert(source.crop_x == 40);
    assert(source.crop_y == 50);
    assert(source.crop_width == 360);
    assert(source.crop_height == 250);

    source.capture_width = 100;
    source.capture_height = 100;
    mmltk::gui::persist_crop_box_to_source(AnnotationBox{90, 95, 140, 160}, &source);
    assert(source.crop_x == 90);
    assert(source.crop_y == 95);
    assert(source.crop_width == 10);
    assert(source.crop_height == 5);

    mmltk::gui::persist_crop_box_to_source(AnnotationBox{-50, -50, 200, 200}, &source);
    assert(source.crop_x == 0);
    assert(source.crop_y == 0);
    assert(source.crop_width == 0);
    assert(source.crop_height == 0);
}

void test_preview_region_for_source_returns_crop_when_full_frame_is_off() {
    SourceSelectionState source = make_video_source();
    source.crop_x = 320;
    source.crop_y = 180;
    source.crop_width = 640;
    source.crop_height = 360;

    const mmltk::live::LiveCaptureRegion texture_region{0U, 0U, 1920U, 1080U};
    const mmltk::live::LiveCaptureRegion crop_region =
        mmltk::gui::preview_region_for_source(texture_region, source, false);
    assert(crop_region.x == 320U);
    assert(crop_region.y == 180U);
    assert(crop_region.width == 640U);
    assert(crop_region.height == 360U);

    const mmltk::live::LiveCaptureRegion full_region =
        mmltk::gui::preview_region_for_source(texture_region, source, true);
    assert(full_region.x == 0U);
    assert(full_region.y == 0U);
    assert(full_region.width == 1920U);
    assert(full_region.height == 1080U);
}

void test_runtime_crop_box_for_ui_state_defaults_to_full_capture_and_clamps_runtime_state() {
    SourceSelectionState source = make_video_source();
    UiCropState ui_crop_state;

    AnnotationBox box = mmltk::gui::runtime_crop_box_for_ui_state(ui_crop_state, source);
    assert(box.x1 == 0);
    assert(box.y1 == 0);
    assert(box.x2 == source.capture_width);
    assert(box.y2 == source.capture_height);

    ui_crop_state.set(mmltk::live::LiveCaptureRegion{1900U, 1000U, 1000U, 500U});
    box = mmltk::gui::runtime_crop_box_for_ui_state(ui_crop_state, source);
    assert(box.x1 == 1900);
    assert(box.y1 == 1000);
    assert(box.x2 == source.capture_width);
    assert(box.y2 == source.capture_height);
}

void test_annotation_save_matching_prefers_canonical_live_frame_identity() {
    AnnotationFrame live_a;
    live_a.frame_id = 17U;
    live_a.live_frame_id = LiveFrameId{1U, 17U};

    AnnotationFrame live_b = live_a;
    live_b.live_frame_id = LiveFrameId{2U, 17U};
    assert(!mmltk::gui::annotation_frames_match_for_save(live_a, live_b));

    live_b.live_frame_id = live_a.live_frame_id;
    assert(mmltk::gui::annotation_frames_match_for_save(live_a, live_b));

    AnnotationFrame plain;
    plain.frame_id = 17U;
    assert(!mmltk::gui::annotation_frame_matches_saved_identity(plain, 17U, live_a.live_frame_id));
    assert(mmltk::gui::annotation_frame_matches_saved_identity(live_a, 999U, live_a.live_frame_id));
    assert(mmltk::gui::annotation_frame_matches_saved_identity(plain, 17U, std::nullopt));
}

void test_annotate_live_session_config_requires_workspace_negotiation() {
    AnnotateViewState annotate;
    annotate.model_input = ModelInputMode::None;
    annotate.weights_path.clear();
    annotate.onnx_path.clear();
    annotate.tensorrt_path.clear();
    annotate.device_id = 2;
    annotate.source.kind = SourceKind::VideoStream;
    annotate.source.device_index = 1;
    annotate.source.capture_width = 1280;
    annotate.source.capture_height = 720;
    annotate.source.capture_fps = 60;
    annotate.source.v4l2_buffer_count = 4;

    bool rejected = false;
    try {
        (void)mmltk::gui::build_annotation_live_session_config(annotate);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("workspace adapter negotiation is not ready") != std::string::npos;
    }
    assert(rejected);
}

void test_live_pipeline_trace_splits_startup_and_post_startup_acquisition_misses() {
    mmltk::live::LivePipelineTrace trace;

    trace.note_acquire_miss();
    mmltk::live::LivePipelineTraceSnapshot snapshot = trace.snapshot();
    assert(snapshot.acquire_misses == 1U);
    assert(snapshot.startup_acquire_misses == 1U);
    assert(snapshot.post_startup_acquire_misses == 0U);

    trace.mark_first_workspace_publication_ready();
    trace.note_acquire_miss();
    snapshot = trace.snapshot();
    assert(snapshot.acquire_misses == 2U);
    assert(snapshot.startup_acquire_misses == 1U);
    assert(snapshot.post_startup_acquire_misses == 1U);
}

}  

MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]", test_seed_runtime_crop_from_source_uses_persisted_crop);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]", test_seed_runtime_crop_from_source_clears_full_capture);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]",
                         test_runtime_crop_mirror_round_trips_partial_crop_and_clears_full_capture);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]",
                         test_persist_crop_box_to_source_round_trips_partial_crop_and_clamps_out_of_bounds);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]",
                         test_preview_region_for_source_returns_crop_when_full_frame_is_off);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]",
                         test_runtime_crop_box_for_ui_state_defaults_to_full_capture_and_clamps_runtime_state);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]",
                         test_annotation_save_matching_prefers_canonical_live_frame_identity);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]", test_annotate_live_session_config_requires_workspace_negotiation);
MMLTK_REGISTER_TEST_CASE("[gui][live_session_utils]",
                         test_live_pipeline_trace_splits_startup_and_post_startup_acquisition_misses);
