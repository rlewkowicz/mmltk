#pragma once

#include "gui/model_input_ui.h"
#include "gui/source_selection.h"
#include "gui/view_state.h"

#include <cstddef>
#include <cstdint>
#include <functional>

struct ImFont;

namespace mmltk::gui {

enum class AnnotationSetupBrowseRequest : std::uint8_t {
    None = 0,
    SingleImage = 1,
    Weights = 2,
    Onnx = 3,
    TensorRt = 4,
};

struct AnnotationSetupTabState {
    AnnotateViewState* annotate = nullptr;
    bool live_video = false;
    bool live_running = false;
    bool block_actions = false;
    bool can_use_video = false;
    bool prepare_running = false;
    bool single_image_browse_busy = false;
    bool weights_browse_busy = false;
    bool onnx_browse_busy = false;
    bool tensorrt_browse_busy = false;
    std::size_t current_input_index = 0;
    std::size_t input_count = 0;
    ImFont* compact_font = nullptr;
};

struct AnnotationSetupTabActions {
    AnnotationSetupBrowseRequest browse_request = AnnotationSetupBrowseRequest::None;
    bool reset_canvas_interactions = false;
    bool preview_invalidated = false;
    bool request_start_live = false;
    bool request_stop_live = false;
    bool request_reload_frame = false;
    bool request_prev_frame = false;
    bool request_next_frame = false;
};

using AnnotationSetupPresetDrawer = std::function<void()>;

[[nodiscard]] AnnotationSetupTabActions draw_annotation_setup_tab(
    AnnotationSetupTabState state,
    const AnnotationSetupPresetDrawer& draw_preset_selector);

} // namespace mmltk::gui
