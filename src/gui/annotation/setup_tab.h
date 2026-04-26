#pragma once

#include <cstdint>

namespace mmltk::gui {

enum class AnnotationSetupBrowseRequest : std::uint8_t {
    None = 0,
    SingleImage = 1,
    Weights = 2,
    Onnx = 3,
    TensorRt = 4,
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

}  // namespace mmltk::gui
