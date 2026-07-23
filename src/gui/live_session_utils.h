#pragma once

#include "annotation_core.h"
#include "source_selection.h"
#include "view_state.h"

#include "mmltk/live/crop_state.h"
#include "mmltk/live/live_capture_region.h"
#include "mmltk/live/live_session_controller.h"

#include <cstdint>
#include <optional>

namespace mmltk::gui {

mmltk::live::LiveCaptureRegion full_capture_region_for_source(const SourceSelectionState& source);
mmltk::live::LiveCaptureRegion clamp_region_to_capture(mmltk::live::LiveCaptureRegion region,
                                                       const SourceSelectionState& source);
AnnotationBox box_from_live_region(const mmltk::live::LiveCaptureRegion& region);
AnnotationBox full_capture_box_for_source(const SourceSelectionState& source);
bool crop_box_is_full_capture(const AnnotationBox& box, const SourceSelectionState& source);
mmltk::live::LiveCaptureRegion preview_region_for_source(const mmltk::live::LiveCaptureRegion& texture_region,
                                                         const SourceSelectionState& source, bool full_frame);
void clear_persisted_video_crop(SourceSelectionState* source);
void persist_crop_box_to_source(const AnnotationBox& box, SourceSelectionState* source);
void publish_runtime_crop_box(mmltk::live::UiCropState& ui_crop_state, const SourceSelectionState& source,
                              const AnnotationBox& box);
void seed_runtime_crop_from_source(mmltk::live::UiCropState& ui_crop_state, const SourceSelectionState& source);
void mirror_runtime_crop_snapshot_to_source(const mmltk::live::UiCropSnapshot& snapshot, SourceSelectionState* source);
void mirror_runtime_crop_to_source(const mmltk::live::UiCropState& ui_crop_state, SourceSelectionState* source);
AnnotationBox runtime_crop_box_for_ui_state(const mmltk::live::UiCropState& ui_crop_state,
                                            const SourceSelectionState& source);
bool annotation_frames_match_for_save(const AnnotationFrame& lhs, const AnnotationFrame& rhs);
bool annotation_frame_matches_saved_identity(const AnnotationFrame& frame, std::uint64_t saved_frame_id,
                                             const std::optional<mmltk::live::LiveFrameId>& saved_live_frame_id);
mmltk::live::LiveSessionConfig build_annotation_live_session_config(const AnnotateViewState& annotate);

}  
