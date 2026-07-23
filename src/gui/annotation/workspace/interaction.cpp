#include "gui/annotation/workspace/interaction.h"

#include "gui/annotation/editor.h"
#include "gui/annotation/projected_scene_cache.h"
#include "gui/annotation_core.h"
#include "gui/live_session_utils.h"
#include "gui/preview_rect_drag.h"

#include <algorithm>
#include <cmath>

namespace mmltk::gui {

namespace {

constexpr int kCreateCommitMinExtent = 6;
constexpr int kCropCommitMinExtent = 32;
constexpr float kMinHandleHitRadiusPx = 10.0f;
constexpr float kMaxHandleHitRadiusPx = 24.0f;
const std::vector<AnnotationVisibleObject> kEmptyVisibleObjects{};
const std::vector<AnnotationEditableHandle> kEmptyEditableHandles{};

bool selected_object_supports_mask_editing(const AnnotationDocument& document, const AnnotationSession& session) {
    const std::optional<std::size_t> selected_index = normalize_selected_object_index(document, session);
    if (!selected_index.has_value()) {
        return false;
    }
    const AnnotationObject* object = document.object(*selected_index);
    return object != nullptr && annotation_object_supports_mask_editing(*object);
}

void update_brush_preview(AnnotationSession& session, const bool selected_supports_mask_editing,
                          const AnnotationWorkspaceInteractionConfig& config) {
    const bool visible = config.overlay_hovered && selected_supports_mask_editing &&
                         annotation_tool_kind_uses_brush(session.active_tool());
    if (!visible) {
        session.set_brush_preview(AnnotationBrushPreview{});
        return;
    }
    session.set_brush_preview(AnnotationBrushPreview{
        visible,
        config.capture_x,
        config.capture_y,
        std::max(1, config.brush_radius),
        session.active_tool() == AnnotationToolKind::MaskErase,
    });
}

std::optional<AnnotationEditableHandle> hit_test_editable_handle(const std::vector<AnnotationEditableHandle>& handles,
                                                                 const AnnotationCanvasLayout& canvas_layout,
                                                                 const CanvasPointerState& canvas_pointer) {
    if (handles.empty()) {
        return std::nullopt;
    }

    const float hit_radius = std::clamp(canvas_layout.state.scale * 4.0f, kMinHandleHitRadiusPx, kMaxHandleHitRadiusPx);
    const float hit_radius_sq = hit_radius * hit_radius;
    const AnnotationEditableHandle* best_handle = nullptr;
    float best_distance_sq = hit_radius_sq;
    for (const AnnotationEditableHandle& handle : handles) {
        const float handle_screen_x = canvas_layout.image_screen_x + handle.frame_point.x * canvas_layout.state.scale;
        const float handle_screen_y = canvas_layout.image_screen_y + handle.frame_point.y * canvas_layout.state.scale;
        const float dx = handle_screen_x - canvas_pointer.screen_x;
        const float dy = handle_screen_y - canvas_pointer.screen_y;
        const float distance_sq = dx * dx + dy * dy;
        if (distance_sq > best_distance_sq) {
            continue;
        }
        best_distance_sq = distance_sq;
        best_handle = &handle;
    }
    return best_handle != nullptr ? std::optional<AnnotationEditableHandle>{*best_handle} : std::nullopt;
}

}  

AnnotationWorkspaceInteractionResult process_annotation_workspace_interaction(
    const AnnotationWorkspaceInteractionRequest& request, const AnnotationWorkspaceInteractionConfig& config) {
    AnnotationDocument& document = request.document;
    AnnotationSession& session = request.session;
    AnnotationController& controller = request.controller;
    AnnotationCategories& categories = request.categories;
    const AnnotationFrame& frame = request.frame;
    SourceSelectionState& source = request.source;
    const AnnotationWorkspaceViewModel& workspace_view = request.workspace_view;
    const AnnotationCanvasLayout& canvas_layout = request.canvas_layout;
    const CanvasPointerState& canvas_pointer = request.canvas_pointer;
    AnnotationWorkspaceInteractionResult result;

    PreviewRectDragSession& create_drag_session = session.create_drag_session();
    PreviewRectDragSession& crop_drag_session = session.crop_drag_session();
    PreviewRectDragSession& direct_drag_session = session.direct_drag_session();
    std::shared_ptr<const AnnotationProjectedScene> interaction_projected_scene = workspace_view.projected_scene;
    const auto ensure_projected_scene_up_to_date =
        [&](const std::optional<std::size_t> selected_object_index) -> const AnnotationProjectedScene* {
        interaction_projected_scene =
            refresh_annotation_projected_scene(frame, document, interaction_projected_scene, selected_object_index);
        return interaction_projected_scene.get();
    };
    const std::optional<std::size_t> initial_selected_object_index = normalize_selected_object_index(document, session);
    const AnnotationProjectedScene* overlay_projected_scene =
        ensure_projected_scene_up_to_date(initial_selected_object_index);
    const std::vector<AnnotationVisibleObject>& workspace_visible_objects =
        overlay_projected_scene != nullptr ? overlay_projected_scene->visible_objects : kEmptyVisibleObjects;
    const std::vector<AnnotationEditableHandle>& workspace_editable_handles =
        overlay_projected_scene != nullptr ? overlay_projected_scene->editable_handles : kEmptyEditableHandles;

    const std::optional<AnnotationVisibleObjectHit> hovered_box_hit =
        config.overlay_hovered && !direct_drag_session.active
            ? AnnotationRenderer::hit_test_visible_objects(workspace_visible_objects, canvas_layout.viewport,
                                                           canvas_pointer, config.image_x, config.image_y,
                                                           session.active_tool() == AnnotationToolKind::Direct)
            : std::nullopt;
    const std::optional<AnnotationVisibleObject> hovered_box =
        hovered_box_hit.has_value() ? std::optional<AnnotationVisibleObject>{hovered_box_hit->object} : std::nullopt;
    const RectDragKind hovered_box_drag_kind =
        hovered_box_hit.has_value() ? hovered_box_hit->drag_kind : RectDragKind::None;

    const bool handle_drag_enabled = session.active_tool() == AnnotationToolKind::Direct &&
                                     !create_drag_session.active && !crop_drag_session.active &&
                                     !direct_drag_session.active && !session.paint_stroke().active;
    const std::optional<AnnotationEditableHandle> hovered_handle =
        handle_drag_enabled && config.overlay_hovered
            ? hit_test_editable_handle(workspace_editable_handles, canvas_layout, canvas_pointer)
            : std::nullopt;
    session.set_hovered_handle(hovered_handle.has_value() ? std::optional<AnnotationHandleId>{hovered_handle->id}
                                                          : std::nullopt);

    if (hovered_box.has_value()) {
        result.hovered_object_index = hovered_box->index;
    }

    std::optional<AnnotationBox> crop_box = workspace_view.crop_frame_box;
    if (!crop_box.has_value()) {
        crop_drag_session = {};
    }
    const AnnotationBox active_crop_box =
        crop_drag_session.active ? crop_drag_session.draft_box : crop_box.value_or(AnnotationBox{});
    const bool crop_drag_enabled = crop_box.has_value() && session.active_tool() == AnnotationToolKind::Direct &&
                                   !create_drag_session.active && !direct_drag_session.active &&
                                   !session.paint_stroke().active;
    const RectDragKind hovered_crop_kind =
        crop_drag_enabled && config.overlay_hovered && !hovered_box.has_value()
            ? rectangle_hover_kind_with_options(canvas_pointer, canvas_layout.viewport, active_crop_box, true,
                                                config.crop_edge_hit_half_width, config.crop_corner_hit_size)
            : RectDragKind::None;

    if (!config.color_sampled && config.overlay_hovered && config.overlay_left_clicked && hovered_box.has_value()) {
        select_object(session, document, hovered_box->index);
    }

    if (config.overlay_left_clicked) {
        if (hovered_handle.has_value()) {
            select_object(session, document, hovered_handle->id.object_index);
            session.begin_handle_drag(hovered_handle->id);
            document.begin_transaction();
        } else if (session.active_tool() == AnnotationToolKind::Direct && hovered_box.has_value() &&
                   hovered_box_drag_kind != RectDragKind::None && hovered_crop_kind == RectDragKind::None) {
            select_object(session, document, hovered_box->index);
            session.set_direct_drag_index(hovered_box->index);
            start_preview_rect_drag(direct_drag_session, hovered_box_drag_kind, canvas_pointer.screen_x,
                                    canvas_pointer.screen_y, hovered_box->frame_box, 1);
        } else if (session.active_tool() == AnnotationToolKind::Box && !create_drag_session.active) {
            start_preview_rect_drag(create_drag_session, RectDragKind::Create, canvas_pointer.screen_x,
                                    canvas_pointer.screen_y,
                                    AnnotationBox{
                                        config.image_x,
                                        config.image_y,
                                        config.image_x + 1,
                                        config.image_y + 1,
                                    },
                                    kCreateCommitMinExtent);
        } else if (hovered_crop_kind != RectDragKind::None) {
            start_preview_rect_drag(crop_drag_session, hovered_crop_kind, canvas_pointer.screen_x,
                                    canvas_pointer.screen_y, active_crop_box, kCropCommitMinExtent);
        }
    }

    if (config.overlay_left_clicked && hovered_crop_kind == RectDragKind::None && !session.handle_drag().has_value() &&
        !direct_drag_session.active && !create_drag_session.active &&
        controller.handle_canvas_click(document, session, frame, categories, config.capture_x, config.capture_y,
                                       config.overlay_left_double_clicked)) {
        result.preview_invalidated = true;
    }

    PreviewRectDragResult crop_drag_frame;
    if (crop_drag_session.active) {
        crop_drag_frame =
            update_preview_rect_drag(crop_drag_session, config.overlay_left_down, canvas_pointer.screen_x,
                                     canvas_pointer.screen_y, canvas_layout.viewport, static_cast<int>(frame.width),
                                     static_cast<int>(frame.height), kCropCommitMinExtent);
        if (crop_drag_frame.commit) {
            persist_crop_box_to_source(annotation_box_from_frame(frame, crop_drag_frame.box), &source);
            result.preview_invalidated = true;
        }
    }

    const std::optional<std::size_t> direct_drag_target_index = session.direct_drag_index();
    PreviewRectDragResult direct_drag_frame;
    if (direct_drag_session.active && direct_drag_target_index.has_value()) {
        const RectDragKind direct_drag_kind = direct_drag_session.drag.kind;
        direct_drag_frame = update_preview_rect_drag(
            direct_drag_session, config.overlay_left_down, canvas_pointer.screen_x, canvas_pointer.screen_y,
            canvas_layout.viewport, static_cast<int>(frame.width), static_cast<int>(frame.height), 1);
        if (direct_drag_frame.commit) {
            const AnnotationBox updated_capture = annotation_box_from_frame(frame, direct_drag_frame.box);
            if (controller.handle_box_commit(document, session, frame, categories, direct_drag_kind, updated_capture,
                                             0U)) {
                result.preview_invalidated = true;
            }
        }
        if (!direct_drag_session.active) {
            session.set_direct_drag_index(std::nullopt);
        }
    }

    PreviewRectDragResult create_drag_frame;
    if (create_drag_session.active) {
        create_drag_frame = update_preview_rect_drag(
            create_drag_session, config.overlay_left_down, canvas_pointer.screen_x, canvas_pointer.screen_y,
            canvas_layout.viewport, static_cast<int>(frame.width), static_cast<int>(frame.height), 1);
        if (create_drag_frame.commit) {
            if (categories.items.empty()) {
                (void)ensure_annotation_category(categories, "object");
            }
            const std::optional<std::size_t> selected_index = normalize_selected_object_index(document, session);
            const AnnotationObject* selected_object =
                selected_index.has_value() ? document.object(*selected_index) : nullptr;
            std::size_t category_index = selected_object != nullptr ? selected_object->category_index : 0U;
            if (!categories.items.empty()) {
                category_index = std::min(category_index, categories.items.size() - 1U);
            }
            const AnnotationBox capture_box = annotation_box_from_frame(frame, create_drag_frame.box);
            if (controller.handle_box_commit(document, session, frame, categories, RectDragKind::Create, capture_box,
                                             category_index)) {
                controller.reset_active_drawing(session);
                result.preview_invalidated = true;
            }
        }
    }

    const std::optional<AnnotationHandleDragState> handle_drag = session.handle_drag();
    if (!config.overlay_left_down) {
        if (handle_drag.has_value() && handle_drag->active) {
            (void)document.commit_transaction();
        }
        session.clear_handle_drag();
    } else if (handle_drag.has_value() && handle_drag->active &&
               controller.handle_handle_drag(document, session, frame, categories, handle_drag->handle,
                                             config.capture_x, config.capture_y)) {
        result.preview_invalidated = true;
    }

    update_brush_preview(session, selected_object_supports_mask_editing(document, session), config);
    const bool paint_mode_active = selected_object_supports_mask_editing(document, session) &&
                                   annotation_tool_kind_uses_brush(session.active_tool());
    const std::optional<std::size_t> selected_index = normalize_selected_object_index(document, session);
    if (paint_mode_active && selected_index.has_value()) {
        if (!config.overlay_left_down) {
            if (session.paint_stroke().active) {
                (void)document.commit_transaction();
            }
            session.clear_paint_stroke();
        } else if (config.overlay_hovered && !config.color_sampled &&
                   (config.overlay_left_clicked || session.paint_stroke().active)) {
            bool changed = false;
            if (!session.paint_stroke().active) {
                session.begin_paint_stroke(config.capture_x, config.capture_y);
                document.begin_transaction();
            }
            rasterize_line_samples(session.paint_stroke().last_capture_x, session.paint_stroke().last_capture_y,
                                   config.capture_x, config.capture_y, [&](const int sample_x, const int sample_y) {
                                       changed =
                                           controller.handle_brush_sample(document, session, frame, categories,
                                                                          sample_x, sample_y, config.brush_radius) ||
                                           changed;
                                   });
            session.update_paint_stroke_position(config.capture_x, config.capture_y);
            if (changed) {
                result.preview_invalidated = true;
            }
        }
    } else {
        if (session.paint_stroke().active) {
            (void)document.commit_transaction();
        }
        session.clear_paint_stroke();
    }

    if (!config.color_sampled && session.active_tool() == AnnotationToolKind::MaskFill &&
        selected_object_supports_mask_editing(document, session) && selected_index.has_value() &&
        config.overlay_hovered && config.overlay_left_clicked &&
        controller.handle_fill(document, session, frame, categories, config.capture_x, config.capture_y)) {
        result.preview_invalidated = true;
    }

    const std::optional<AnnotationHandleDragState> current_handle_drag = session.handle_drag();
    const bool handle_drag_active = current_handle_drag.has_value() && current_handle_drag->active;
    session.set_pointer_captured(session.create_drag_session().active || session.crop_drag_session().active ||
                                 session.direct_drag_session().active || handle_drag_active ||
                                 session.paint_stroke().active);

    if (handle_drag_active) {
        result.cursor_kind = RectDragKind::Move;
    } else if (session.direct_drag_session().active) {
        result.cursor_kind = session.direct_drag_session().drag.kind;
    } else if (session.crop_drag_session().active) {
        result.cursor_kind = session.crop_drag_session().drag.kind;
    } else {
        if (hovered_handle.has_value()) {
            result.cursor_kind = RectDragKind::Move;
        } else if (session.active_tool() == AnnotationToolKind::Direct && hovered_box_drag_kind != RectDragKind::None) {
            result.cursor_kind = hovered_box_drag_kind;
        } else if (hovered_crop_kind != RectDragKind::None) {
            result.cursor_kind = hovered_crop_kind;
        }
    }

    const std::optional<std::size_t> selected_object_index = normalize_selected_object_index(document, session);
    overlay_projected_scene = ensure_projected_scene_up_to_date(selected_object_index);

    const std::optional<std::size_t> replaced_box_index = session.direct_drag_session().active
                                                              ? session.direct_drag_index()
                                                          : direct_drag_frame.commit ? direct_drag_target_index
                                                                                     : std::optional<std::size_t>{};
    std::optional<AnnotationBox> drag_box;
    if (replaced_box_index.has_value()) {
        drag_box = session.direct_drag_session().active
                       ? std::optional<AnnotationBox>{session.direct_drag_session().draft_box}
                   : direct_drag_frame.commit ? std::optional<AnnotationBox>{direct_drag_frame.box}
                                              : std::nullopt;
    }
    const std::optional<AnnotationBox> create_box =
        session.create_drag_session().active ? std::optional<AnnotationBox>{session.create_drag_session().draft_box}
        : create_drag_frame.commit           ? std::optional<AnnotationBox>{create_drag_frame.box}
                                             : std::nullopt;
    const std::optional<AnnotationBox> render_crop_box =
        session.crop_drag_session().active ? std::optional<AnnotationBox>{session.crop_drag_session().draft_box}
        : crop_drag_frame.commit && config.live_video && config.full_frame
            ? AnnotationRenderer::project_capture_box(frame, resolved_video_crop_box(source)).frame_box
            : workspace_view.crop_frame_box;
    const std::optional<AnnotationHandleId> dragged_handle =
        handle_drag_active ? std::optional<AnnotationHandleId>{current_handle_drag->handle} : std::nullopt;
    result.overlay_request = AnnotationRenderer::build_interaction_overlay_request(
        frame, config.cuda_device_index, *overlay_projected_scene, result.hovered_object_index, replaced_box_index,
        render_crop_box, std::max(1, config.crop_handle_radius), drag_box, create_box, session.hovered_handle(),
        dragged_handle);
    result.projected_scene = std::move(interaction_projected_scene);
    return result;
}

}  
