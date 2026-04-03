#include "gui/annotation/setup_tab.h"

#include "gui/annotation/annotation_ui.h"
#include "gui/ui_controls.h"

#include <imgui.h>

namespace mmltk::gui {

AnnotationSetupTabActions draw_annotation_setup_tab(
    AnnotationSetupTabState state,
    const AnnotationSetupPresetDrawer& draw_preset_selector) {
    AnnotationSetupTabActions actions;
    if (state.annotate == nullptr) {
        return actions;
    }
    AnnotateViewState& annotate = *state.annotate;

    if (draw_preset_selector) {
        draw_preset_selector();
    }

    draw_section_heading("Source");
    SourceSelectionUiActions source_actions{};
    {
        [[maybe_unused]] const annotation_ui::DisabledScope disabled_scope(state.live_running);
        source_actions =
            draw_source_selection(annotate.source,
                                  "annotate_source",
                                  false,
                                  true,
                                  true,
                                  true,
                                  state.single_image_browse_busy);
    }
    if (source_actions.browse_single_image) {
        actions.browse_request = AnnotationSetupBrowseRequest::SingleImage;
    }

    draw_section_heading("Model");
    if (state.live_running) {
        annotation_ui::draw_compact_disabled_hint(
            state.compact_font,
            "Source and model settings stay locked while live annotate is running.");
    }
    switch (draw_model_input_selector(annotate.model_input,
                                      annotate.weights_path,
                                      annotate.onnx_path,
                                      annotate.tensorrt_path,
                                      state.weights_browse_busy,
                                      state.onnx_browse_busy,
                                      state.tensorrt_browse_busy,
                                      true)) {
    case ModelInputBrowseRequest::Weights:
        actions.browse_request = AnnotationSetupBrowseRequest::Weights;
        break;
    case ModelInputBrowseRequest::Onnx:
        actions.browse_request = AnnotationSetupBrowseRequest::Onnx;
        break;
    case ModelInputBrowseRequest::TensorRt:
        actions.browse_request = AnnotationSetupBrowseRequest::TensorRt;
        break;
    case ModelInputBrowseRequest::None:
        break;
    }

    draw_full_width_input("Backend Preference", annotate.backend);
    if (annotate.model_input == ModelInputMode::None) {
        annotation_ui::draw_compact_disabled_hint(
            state.compact_font,
            "Manual annotate mode uses the full bounding box as the seed mask. Use Suppress and No Suppress to carve out the final mask.");
    }

    if (state.live_video) {
        draw_section_heading("Preview");
        const bool previous_full_frame = annotate.full_frame;
        draw_labeled_checkbox("Full Frame", annotate.full_frame);
        if (annotate.full_frame != previous_full_frame) {
            actions.reset_canvas_interactions = true;
            actions.preview_invalidated = true;
        }
        annotation_ui::draw_compact_wrapped_text(
            state.compact_font,
            "Preview only. Off shows the crop itself. On shows full capture with the crop box overlaid. This does not change runtime capture.");
    }

    draw_section_heading("Execution");
    {
        [[maybe_unused]] const annotation_ui::DisabledScope disabled_scope(state.live_running);
        draw_labeled_int_input("Device ID", annotate.device_id, 140.0f);
        draw_labeled_int_input("Max Dets", annotate.max_dets_per_image, 140.0f);
        draw_labeled_float_input("Threshold", annotate.threshold, 140.0f, 0.01f, 0.10f, "%.3f");
        draw_labeled_checkbox("FP16", annotate.allow_fp16);
        (void)draw_compile_mode_combo("Compile Mode", annotate.compile_mode);
    }

    draw_section_heading("Frame");
    if (state.live_video) {
        const bool can_start_live = !state.block_actions && state.can_use_video;
        if (!state.live_running) {
            if (annotation_ui::draw_primary_button("Start Live Annotate", can_start_live)) {
                actions.request_start_live = true;
            }
        } else if (annotation_ui::draw_danger_button("Stop Live Annotate")) {
            actions.request_stop_live = true;
        }
        if (!state.can_use_video) {
            annotation_ui::draw_compact_wrapped_text(
                state.compact_font,
                annotation_ui::TextTone::Warning,
                "Live video annotate is not available in this build.");
        }
        return actions;
    }

    const bool has_inputs = state.input_count > 0U;
    if (const auto pressed =
            annotation_ui::draw_inline_button_row(
                std::array{
                    annotation_ui::button_token("Reload", has_inputs),
                    annotation_ui::button_token("Prev", has_inputs),
                    annotation_ui::button_token("Next", has_inputs),
                })) {
        switch (*pressed) {
        case 0U:
            actions.request_reload_frame = true;
            break;
        case 1U:
            actions.request_prev_frame = true;
            break;
        case 2U:
            actions.request_next_frame = true;
            break;
        default:
            break;
        }
    }

    if (has_inputs) {
        annotation_ui::draw_compact_wrapped_text(state.compact_font,
                                                 "Image %zu / %zu",
                                                 state.current_input_index + 1U,
                                                 state.input_count);
    } else if (state.prepare_running) {
        annotation_ui::draw_compact_text(state.compact_font, "Scanning source...");
    }

    return actions;
}

} // namespace mmltk::gui
