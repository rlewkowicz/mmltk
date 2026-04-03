#include "gui/annotation/workspace/canvas.h"

#include "gui/annotation_core.h"

#include <algorithm>

namespace mmltk::gui {

namespace {

constexpr float kMinCanvasScale = 0.05f;
constexpr float kMaxCanvasScale = 64.0f;
constexpr float kFocusPadding = 48.0f;

float clamped_canvas_scale(const float scale) {
    return std::clamp(scale, kMinCanvasScale, kMaxCanvasScale);
}

float fit_scale_for_frame(const AnnotationFrame& frame,
                          const float available_width,
                          const float available_height) {
    if (frame.width == 0U || frame.height == 0U ||
        available_width <= 0.0f || available_height <= 0.0f) {
        return 1.0f;
    }
    const auto width = static_cast<float>(frame.width);
    const auto height = static_cast<float>(frame.height);
    const float fit_scale = std::min(available_width / width, available_height / height);
    return fit_scale > 0.0f ? fit_scale : 1.0f;
}

AnnotationCanvasState clamp_canvas_state(const AnnotationFrame& frame,
                                         const AnnotationCanvasLayoutInput& input,
                                         const float fit_scale) {
    AnnotationCanvasState state = input.state;
    state.scale =
        state.auto_fit || state.scale <= 0.0f ? fit_scale : clamped_canvas_scale(state.scale);
    const auto image_width = static_cast<float>(frame.width) * state.scale;
    const auto image_height = static_cast<float>(frame.height) * state.scale;
    state.pan_x = clamp_annotation_canvas_pan_axis(state.pan_x, image_width, input.available_width);
    state.pan_y = clamp_annotation_canvas_pan_axis(state.pan_y, image_height, input.available_height);
    return state;
}

AnnotationCanvasState clamp_free_canvas_state(const AnnotationCanvasLayout& layout,
                                              AnnotationCanvasState state) {
    state.auto_fit = false;
    state.scale = clamped_canvas_scale(state.scale);
    const auto frame_width = static_cast<float>(layout.viewport.image_width);
    const auto frame_height = static_cast<float>(layout.viewport.image_height);
    state.pan_x = clamp_annotation_canvas_pan_axis(state.pan_x,
                                                   frame_width * state.scale,
                                                   layout.available_width);
    state.pan_y = clamp_annotation_canvas_pan_axis(state.pan_y,
                                                   frame_height * state.scale,
                                                   layout.available_height);
    return state;
}

} // namespace

float clamp_annotation_canvas_pan_axis(const float pan,
                                       const float image_extent,
                                       const float viewport_extent) {
    if (image_extent <= viewport_extent) {
        return 0.0f;
    }
    return std::clamp(pan, viewport_extent - image_extent, 0.0f);
}

AnnotationCanvasLayout build_annotation_canvas_layout(const AnnotationFrame& frame,
                                                      const AnnotationCanvasLayoutInput& input) {
    AnnotationCanvasLayout layout;
    layout.available_width = std::max(0.0f, input.available_width);
    layout.available_height = std::max(0.0f, input.available_height);
    layout.viewport_screen_x = input.viewport_screen_x;
    layout.viewport_screen_y = input.viewport_screen_y;
    layout.viewport_screen_max_x = layout.viewport_screen_x + layout.available_width;
    layout.viewport_screen_max_y = layout.viewport_screen_y + layout.available_height;
    layout.fit_scale = fit_scale_for_frame(frame, layout.available_width, layout.available_height);
    layout.state = clamp_canvas_state(frame, input, layout.fit_scale);
    layout.image_width = static_cast<float>(frame.width) * layout.state.scale;
    layout.image_height = static_cast<float>(frame.height) * layout.state.scale;

    const float centered_x =
        layout.viewport_screen_x + std::max(0.0f, (layout.available_width - layout.image_width) * 0.5f);
    const float centered_y =
        layout.viewport_screen_y + std::max(0.0f, (layout.available_height - layout.image_height) * 0.5f);
    layout.image_screen_x = centered_x + layout.state.pan_x;
    layout.image_screen_y = centered_y + layout.state.pan_y;
    layout.image_screen_max_x = layout.image_screen_x + layout.image_width;
    layout.image_screen_max_y = layout.image_screen_y + layout.image_height;

    layout.viewport_hovered =
        input.window_hovered &&
        input.mouse_screen_x >= layout.viewport_screen_x &&
        input.mouse_screen_y >= layout.viewport_screen_y &&
        input.mouse_screen_x <= layout.viewport_screen_max_x &&
        input.mouse_screen_y <= layout.viewport_screen_max_y;
    layout.overlay_hovered =
        layout.viewport_hovered &&
        input.mouse_screen_x >= layout.image_screen_x &&
        input.mouse_screen_y >= layout.image_screen_y &&
        input.mouse_screen_x <= layout.image_screen_max_x &&
        input.mouse_screen_y <= layout.image_screen_max_y;

    if (frame.width > 0U && frame.height > 0U && layout.state.scale > 0.0f) {
        const auto max_image_x = static_cast<float>(frame.width) - 0.0001f;
        const auto max_image_y = static_cast<float>(frame.height) - 0.0001f;
        const float local_x = std::clamp((input.mouse_screen_x - layout.image_screen_x) / layout.state.scale,
                                         0.0f,
                                         std::max(0.0f, max_image_x));
        const float local_y = std::clamp((input.mouse_screen_y - layout.image_screen_y) / layout.state.scale,
                                         0.0f,
                                         std::max(0.0f, max_image_y));
        layout.image_x = std::clamp(static_cast<int>(local_x), 0, static_cast<int>(frame.width) - 1);
        layout.image_y = std::clamp(static_cast<int>(local_y), 0, static_cast<int>(frame.height) - 1);
    }

    const std::uint32_t capture_width = annotation_frame_capture_width(frame);
    const std::uint32_t capture_height = annotation_frame_capture_height(frame);
    if (capture_width > 0U) {
        layout.capture_x = std::clamp(static_cast<int>(frame.view_x) + layout.image_x,
                                      0,
                                      static_cast<int>(capture_width) - 1);
    }
    if (capture_height > 0U) {
        layout.capture_y = std::clamp(static_cast<int>(frame.view_y) + layout.image_y,
                                      0,
                                      static_cast<int>(capture_height) - 1);
    }

    layout.viewport = make_canvas_viewport(layout.image_screen_x,
                                           layout.image_screen_y,
                                           layout.image_width,
                                           layout.image_height,
                                           frame.width,
                                           frame.height);
    return layout;
}

AnnotationCanvasState annotation_canvas_fit_state(const AnnotationCanvasLayout& layout) {
    return AnnotationCanvasState{
        layout.fit_scale,
        0.0f,
        0.0f,
        true,
    };
}

AnnotationCanvasState annotation_canvas_one_to_one_state() noexcept {
    return AnnotationCanvasState{
        1.0f,
        0.0f,
        0.0f,
        false,
    };
}

AnnotationCanvasState annotation_canvas_zoom_around_point(const AnnotationCanvasLayout& layout,
                                                          const float new_scale,
                                                          const float anchor_screen_x,
                                                          const float anchor_screen_y) {
    AnnotationCanvasState state = layout.state;
    const auto frame_width = static_cast<float>(layout.viewport.image_width);
    const auto frame_height = static_cast<float>(layout.viewport.image_height);
    if (frame_width <= 0.0f || frame_height <= 0.0f || layout.state.scale <= 0.0f) {
        return clamp_free_canvas_state(layout, state);
    }

    const float clamped_scale = clamped_canvas_scale(new_scale);
    const float image_local_x =
        std::clamp((anchor_screen_x - layout.image_screen_x) / layout.state.scale, 0.0f, frame_width);
    const float image_local_y =
        std::clamp((anchor_screen_y - layout.image_screen_y) / layout.state.scale, 0.0f, frame_height);
    const float new_image_width = frame_width * clamped_scale;
    const float new_image_height = frame_height * clamped_scale;
    const float centered_x =
        layout.viewport_screen_x + std::max(0.0f, (layout.available_width - new_image_width) * 0.5f);
    const float centered_y =
        layout.viewport_screen_y + std::max(0.0f, (layout.available_height - new_image_height) * 0.5f);
    state.scale = clamped_scale;
    state.pan_x = anchor_screen_x - centered_x - image_local_x * clamped_scale;
    state.pan_y = anchor_screen_y - centered_y - image_local_y * clamped_scale;
    return clamp_free_canvas_state(layout, state);
}

AnnotationCanvasState annotation_canvas_pan_by_delta(const AnnotationCanvasLayout& layout,
                                                     const float delta_x,
                                                     const float delta_y) {
    AnnotationCanvasState state = layout.state;
    state.pan_x += delta_x;
    state.pan_y += delta_y;
    return clamp_free_canvas_state(layout, state);
}

AnnotationCanvasState annotation_canvas_focus_box(const AnnotationCanvasLayout& layout,
                                                  const AnnotationBox& frame_box) {
    if (!annotation_box_has_area(frame_box)) {
        return layout.state;
    }

    const auto box_width = static_cast<float>(frame_box.x2 - frame_box.x1);
    const auto box_height = static_cast<float>(frame_box.y2 - frame_box.y1);
    const auto frame_width = static_cast<float>(layout.viewport.image_width);
    const auto frame_height = static_cast<float>(layout.viewport.image_height);
    if (box_width <= 0.0f || box_height <= 0.0f || frame_width <= 0.0f || frame_height <= 0.0f) {
        return layout.state;
    }

    const float focus_scale = clamped_canvas_scale(
        std::min(layout.available_width / (box_width + kFocusPadding),
                 layout.available_height / (box_height + kFocusPadding)));
    const float new_image_width = frame_width * focus_scale;
    const float new_image_height = frame_height * focus_scale;
    const float centered_x =
        layout.viewport_screen_x + std::max(0.0f, (layout.available_width - new_image_width) * 0.5f);
    const float centered_y =
        layout.viewport_screen_y + std::max(0.0f, (layout.available_height - new_image_height) * 0.5f);
    AnnotationCanvasState state;
    state.scale = focus_scale;
    state.auto_fit = false;
    state.pan_x =
        layout.viewport_screen_x + layout.available_width * 0.5f -
        centered_x -
        (static_cast<float>(frame_box.x1 + frame_box.x2) * 0.5f) * focus_scale;
    state.pan_y =
        layout.viewport_screen_y + layout.available_height * 0.5f -
        centered_y -
        (static_cast<float>(frame_box.y1 + frame_box.y2) * 0.5f) * focus_scale;
    return clamp_free_canvas_state(layout, state);
}

} // namespace mmltk::gui
