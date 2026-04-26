#include "live_session_utils.h"

#include "canvas_layers.h"

#include <algorithm>

namespace mmltk::gui {

using mmltk::live::LiveCaptureRegion;

LiveCaptureRegion full_capture_region_for_source(const SourceSelectionState& source) {
    return LiveCaptureRegion{
        0U,
        0U,
        static_cast<std::uint32_t>(std::max(1, source.capture_width)),
        static_cast<std::uint32_t>(std::max(1, source.capture_height)),
    };
}

LiveCaptureRegion clamp_region_to_capture(LiveCaptureRegion region, const SourceSelectionState& source) {
    const std::uint32_t capture_width = static_cast<std::uint32_t>(std::max(1, source.capture_width));
    const std::uint32_t capture_height = static_cast<std::uint32_t>(std::max(1, source.capture_height));
    region.x = std::min(region.x, capture_width - 1U);
    region.y = std::min(region.y, capture_height - 1U);
    region.width = std::clamp(region.width, 1U, capture_width - region.x);
    region.height = std::clamp(region.height, 1U, capture_height - region.y);
    return region;
}

AnnotationBox box_from_live_region(const LiveCaptureRegion& region) {
    return AnnotationBox{
        static_cast<int>(region.x),
        static_cast<int>(region.y),
        static_cast<int>(region.x + region.width),
        static_cast<int>(region.y + region.height),
    };
}

AnnotationBox full_capture_box_for_source(const SourceSelectionState& source) {
    return box_from_live_region(full_capture_region_for_source(source));
}

bool crop_box_is_full_capture(const AnnotationBox& box, const SourceSelectionState& source) {
    const AnnotationBox normalized =
        normalize_annotation_box(box, static_cast<std::uint32_t>(std::max(1, source.capture_width)),
                                 static_cast<std::uint32_t>(std::max(1, source.capture_height)));
    const AnnotationBox full = full_capture_box_for_source(source);
    return normalized.x1 == full.x1 && normalized.y1 == full.y1 && normalized.x2 == full.x2 && normalized.y2 == full.y2;
}

mmltk::live::LiveCaptureRegion preview_region_for_source(const LiveCaptureRegion& texture_region,
                                                         const SourceSelectionState& source, const bool full_frame) {
    if (texture_region.width == 0U || texture_region.height == 0U || full_frame ||
        source.kind != SourceKind::VideoStream) {
        return texture_region;
    }

    const AnnotationBox crop_box = resolved_video_crop_box(source);
    const int tex_x1 = static_cast<int>(texture_region.x);
    const int tex_y1 = static_cast<int>(texture_region.y);
    const int tex_x2 = tex_x1 + static_cast<int>(texture_region.width);
    const int tex_y2 = tex_y1 + static_cast<int>(texture_region.height);
    const int crop_x1 = std::clamp(crop_box.x1, tex_x1, std::max(tex_x1, tex_x2 - 1));
    const int crop_y1 = std::clamp(crop_box.y1, tex_y1, std::max(tex_y1, tex_y2 - 1));
    const int crop_x2 = std::clamp(crop_box.x2, crop_x1 + 1, tex_x2);
    const int crop_y2 = std::clamp(crop_box.y2, crop_y1 + 1, tex_y2);
    return LiveCaptureRegion{
        static_cast<std::uint32_t>(crop_x1),
        static_cast<std::uint32_t>(crop_y1),
        static_cast<std::uint32_t>(std::max(1, crop_x2 - crop_x1)),
        static_cast<std::uint32_t>(std::max(1, crop_y2 - crop_y1)),
    };
}

void clear_persisted_video_crop(SourceSelectionState* source) {
    if (source == nullptr) {
        return;
    }
    source->crop_x = 0;
    source->crop_y = 0;
    source->crop_width = 0;
    source->crop_height = 0;
}

void persist_crop_box_to_source(const AnnotationBox& box, SourceSelectionState* source) {
    if (source == nullptr) {
        return;
    }

    const AnnotationBox normalized =
        normalize_annotation_box(box, static_cast<std::uint32_t>(std::max(1, source->capture_width)),
                                 static_cast<std::uint32_t>(std::max(1, source->capture_height)));
    if (!annotation_box_has_area(normalized) || crop_box_is_full_capture(normalized, *source)) {
        clear_persisted_video_crop(source);
        return;
    }

    (void)assign_video_crop_box(*source, normalized);
}

void publish_runtime_crop_box(mmltk::live::UiCropState& ui_crop_state, const SourceSelectionState& source,
                              const AnnotationBox& box) {
    const AnnotationBox normalized =
        normalize_annotation_box(box, static_cast<std::uint32_t>(std::max(1, source.capture_width)),
                                 static_cast<std::uint32_t>(std::max(1, source.capture_height)));
    if (!annotation_box_has_area(normalized) || crop_box_is_full_capture(normalized, source)) {
        ui_crop_state.clear();
        return;
    }

    ui_crop_state.set(clamp_region_to_capture(
        LiveCaptureRegion{
            static_cast<std::uint32_t>(normalized.x1),
            static_cast<std::uint32_t>(normalized.y1),
            static_cast<std::uint32_t>(std::max(1, normalized.x2 - normalized.x1)),
            static_cast<std::uint32_t>(std::max(1, normalized.y2 - normalized.y1)),
        },
        source));
}

void seed_runtime_crop_from_source(mmltk::live::UiCropState& ui_crop_state, const SourceSelectionState& source) {
    publish_runtime_crop_box(ui_crop_state, source, resolved_video_crop_box(source));
}

void mirror_runtime_crop_snapshot_to_source(const mmltk::live::UiCropSnapshot& snapshot, SourceSelectionState* source) {
    if (source == nullptr) {
        return;
    }
    if (!snapshot.has_crop) {
        clear_persisted_video_crop(source);
        return;
    }

    const AnnotationBox crop_box =
        normalize_annotation_box(box_from_live_region(clamp_region_to_capture(snapshot.region, *source)),
                                 static_cast<std::uint32_t>(std::max(1, source->capture_width)),
                                 static_cast<std::uint32_t>(std::max(1, source->capture_height)));
    if (crop_box_is_full_capture(crop_box, *source)) {
        clear_persisted_video_crop(source);
        return;
    }

    persist_crop_box_to_source(crop_box, source);
}

void mirror_runtime_crop_to_source(const mmltk::live::UiCropState& ui_crop_state, SourceSelectionState* source) {
    const auto snapshot = ui_crop_state.snapshot();
    if (!snapshot) {
        clear_persisted_video_crop(source);
        return;
    }
    mirror_runtime_crop_snapshot_to_source(*snapshot, source);
}

AnnotationBox runtime_crop_box_for_ui_state(const mmltk::live::UiCropState& ui_crop_state,
                                            const SourceSelectionState& source) {
    const auto snapshot = ui_crop_state.snapshot();
    if (!snapshot || !snapshot->has_crop) {
        return full_capture_box_for_source(source);
    }

    return normalize_annotation_box(box_from_live_region(clamp_region_to_capture(snapshot->region, source)),
                                    static_cast<std::uint32_t>(std::max(1, source.capture_width)),
                                    static_cast<std::uint32_t>(std::max(1, source.capture_height)));
}

bool annotation_frames_match_for_save(const AnnotationFrame& lhs, const AnnotationFrame& rhs) {
    if (lhs.live_frame_id.has_value() || rhs.live_frame_id.has_value()) {
        return lhs.live_frame_id.has_value() && rhs.live_frame_id.has_value() &&
               *lhs.live_frame_id == *rhs.live_frame_id;
    }
    return lhs.frame_id == rhs.frame_id;
}

bool annotation_frame_matches_saved_identity(const AnnotationFrame& frame, const std::uint64_t saved_frame_id,
                                             const std::optional<mmltk::live::LiveFrameId>& saved_live_frame_id) {
    if (frame.live_frame_id.has_value() || saved_live_frame_id.has_value()) {
        return frame.live_frame_id.has_value() && saved_live_frame_id.has_value() &&
               *frame.live_frame_id == *saved_live_frame_id;
    }
    return frame.frame_id == saved_frame_id;
}

}  // namespace mmltk::gui
