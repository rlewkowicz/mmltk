#include "gui/annotation/controller.h"
#include "gui/annotation/document/edit.h"
#include "gui/annotation/document/document.h"
#include "gui/annotation/editor.h"
#include "gui/annotation/editor_history.h"
#include "gui/annotation/projected_scene_cache.h"
#include "gui/annotation/render/renderer.h"
#include "gui/annotation/sidebar_edits.h"
#include "gui/annotation/workflow_ui.h"
#include "gui/annotation/workspace/model.h"

#include "support/catch2_compat.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace mmltk::gui;

template <typename T>
[[nodiscard]] const T& require_optional_ref(const std::optional<T>& value, const std::string_view message) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(message));
    }
    return *value;
}

template <typename T>
[[nodiscard]] T require_optional_value(const std::optional<T>& value, const std::string_view message) {
    return require_optional_ref(value, message);
}

template <typename T>
[[nodiscard]] const T& require_pointer(const T* value, const std::string_view message) {
    if (value == nullptr) {
        throw std::runtime_error(std::string(message));
    }
    return *value;
}

AnnotationObject make_test_annotation_object() {
    AnnotationObject object;
    object.object_id = "manual-1";
    object.category_index = 0U;
    object.shape = AnnotationBoxShape{AnnotationBox{4, 5, 12, 18}};
    return object;
}

AnnotationObject make_test_mask_annotation_object() {
    AnnotationObject object;
    object.object_id = "mask-1";
    object.category_index = 0U;
    object.sup.center.hue_degrees = 16.0f;
    object.nosup.center.hue_degrees = 48.0f;
    object.shape = AnnotationMaskShape{
        AnnotationBox{4, 5, 7, 8},
        AnnotationMaskRegion{4U, 5U, 3U, 3U},
        {
            1U,
            1U,
            1U,
            1U,
            0U,
            1U,
            1U,
            1U,
            1U,
        },
        0U,
        std::nullopt,
    };
    return object;
}

AnnotationObject make_test_spline_annotation_object() {
    AnnotationObject object;
    object.object_id = "spline-1";
    object.category_index = 0U;
    object.shape = AnnotationSplineShape{
        false,
        {
            AnnotationSplineKnot{
                AnnotationPoint{8.0f, 12.0f},
                AnnotationSplineHandle{},
                AnnotationSplineHandle{},
                AnnotationSplineHandleMode::Corner,
            },
            AnnotationSplineKnot{
                AnnotationPoint{28.0f, 12.0f},
                AnnotationSplineHandle{},
                AnnotationSplineHandle{},
                AnnotationSplineHandleMode::Corner,
            },
        },
    };
    return object;
}

AnnotationObject make_test_skeleton_annotation_object() {
    AnnotationObject object;
    object.object_id = "skeleton-1";
    object.category_index = 0U;
    object.shape = AnnotationSkeletonShape{
        {
            AnnotationSkeletonNode{"hip", AnnotationPoint{12.0f, 14.0f}, true},
            AnnotationSkeletonNode{"knee", AnnotationPoint{24.0f, 22.0f}, true},
        },
        {
            AnnotationSkeletonEdge{0U, 1U},
        },
    };
    return object;
}

AnnotationFrame make_annotation_frame(const std::uint32_t width, const std::uint32_t height) {
    AnnotationFrame frame;
    frame.width = width;
    frame.height = height;
    frame.capture_width = width;
    frame.capture_height = height;
    return frame;
}

AnnotationFrame make_render_frame() {
    return make_annotation_frame(96U, 64U);
}

AnnotationObject make_smooth_two_knot_spline_annotation_object(const char* object_id, const std::size_t category_index,
                                                               const bool closed) {
    AnnotationObject object;
    object.object_id = object_id;
    object.category_index = category_index;
    object.shape = AnnotationSplineShape{
        closed,
        {
            AnnotationSplineKnot{
                AnnotationPoint{10.0f, 12.0f},
                AnnotationSplineHandle{},
                AnnotationSplineHandle{AnnotationPoint{18.0f, 12.0f}, true},
                AnnotationSplineHandleMode::Smooth,
            },
            AnnotationSplineKnot{
                AnnotationPoint{28.0f, 18.0f},
                AnnotationSplineHandle{AnnotationPoint{20.0f, 18.0f}, true},
                AnnotationSplineHandle{},
                AnnotationSplineHandleMode::Smooth,
            },
        },
    };
    return object;
}

AnnotationObjectsTabApplyContext make_objects_tab_apply_context(AnnotationController& controller,
                                                                AnnotationDocument& document,
                                                                AnnotationSession& session,
                                                                AnnotationCategories& categories,
                                                                const AnnotationFrame* frame) {
    return AnnotationObjectsTabApplyContext{
        controller, document, session, categories, frame, nullptr, false,
    };
}

std::vector<AnnotationEditableHandle> build_selected_editable_handles(const AnnotationDocument& document,
                                                                      const std::size_t selected_object_index = 0U) {
    return AnnotationRenderer::build_editable_handles(make_render_frame(), document,
                                                      std::optional<std::size_t>{selected_object_index});
}

class TrackingAnnotationTool final : public AnnotationTool {
   public:
    TrackingAnnotationTool(AnnotationToolKind kind, int* reset_count) : kind_(kind), reset_count_(reset_count) {}

    [[nodiscard]] AnnotationToolKind kind() const noexcept override {
        return kind_;
    }

    void reset_active_drawing(AnnotationSession&) override {
        if (reset_count_ == nullptr) {
            throw std::runtime_error("tracking annotation tool requires a reset counter");
        }
        ++(*reset_count_);
    }

   private:
    AnnotationToolKind kind_;
    int* reset_count_ = nullptr;
};

void test_annotation_document_generation_tracks_commands() {
    AnnotationDocument document;
    assert(document.generation() == 0U);
    assert(document.empty());

    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    assert(document.generation() == 1U);
    assert(document.size() == 1U);

    AnnotationObject replaced = *document.object(0);
    replaced.enabled = false;
    replaced.category_index = 3U;
    assert(document.apply(AnnotationReplaceObjectCommand{0U, replaced}));
    assert(document.generation() == 2U);
    assert(document.object(0) != nullptr);
    assert(!document.object(0)->enabled);
    assert(document.object(0)->category_index == 3U);

    assert(!document.apply(AnnotationRemoveObjectCommand{9U}));
    assert(document.generation() == 2U);

    assert(document.apply(AnnotationRemoveObjectCommand{0U}));
    assert(document.generation() == 3U);
    assert(document.empty());
}

void test_annotation_controller_tool_switch_clears_transient_session_state() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;

    session.set_hovered_handle(AnnotationHandleId{2U, 4U, AnnotationHandleRole::SplineKnot});
    session.set_pointer_captured(true);
    session.set_direct_drag_index(3U);
    session.begin_handle_drag(AnnotationHandleId{2U, 4U, AnnotationHandleRole::SplineKnot});
    session.create_drag_session().active = true;
    session.crop_drag_session().active = true;
    session.direct_drag_session().active = true;
    session.begin_paint_stroke(14, 18);
    session.set_brush_preview(AnnotationBrushPreview{
        true,
        10,
        12,
        16,
        true,
    });
    const std::uint64_t revision_before = session.revision();

    assert(controller.set_active_tool(AnnotationToolKind::MaskPaint, document, session));
    assert(session.active_tool() == AnnotationToolKind::MaskPaint);
    assert(session.revision() > revision_before);
    assert(!session.hovered_handle().has_value());
    assert(!session.pointer_captured());
    assert(!session.brush_preview().visible);
    assert(!session.create_drag_session().active);
    assert(!session.crop_drag_session().active);
    assert(!session.direct_drag_session().active);
    assert(!session.direct_drag_index().has_value());
    assert(!session.handle_drag().has_value());
    assert(!session.paint_stroke().active);

    session.set_pointer_captured(true);
    session.create_drag_session().active = true;
    controller.reset_active_drawing(session);
    assert(!session.pointer_captured());
    assert(!session.create_drag_session().active);
}

void test_annotation_session_overlay_revision_tracks_selection_only() {
    AnnotationSession session;
    assert(session.revision() == 0U);
    assert(session.overlay_revision() == 0U);

    session.set_pointer_captured(true);
    assert(session.revision() == 1U);
    assert(session.overlay_revision() == 0U);

    session.select_object(2U);
    assert(session.overlay_revision() == 1U);
    const std::uint64_t overlay_revision = session.overlay_revision();
    const std::uint64_t revision = session.revision();

    session.set_brush_preview(AnnotationBrushPreview{
        true,
        10,
        12,
        16,
        false,
    });
    assert(session.revision() > revision);
    assert(session.overlay_revision() == overlay_revision);

    session.select_object(2U);
    assert(session.overlay_revision() == overlay_revision);
}

void test_annotation_document_metadata_commands_update_generation() {
    AnnotationDocument document;
    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    assert(document.generation() == 1U);

    assert(document.apply(AnnotationSetObjectEnabledCommand{0U, false}));
    assert(document.generation() == 2U);
    assert(document.object(0U) != nullptr);
    assert(!document.object(0U)->enabled);
    assert(!document.apply(AnnotationSetObjectEnabledCommand{0U, false}));
    assert(document.generation() == 2U);

    assert(document.apply(AnnotationSetObjectCategoryCommand{0U, 4U}));
    assert(document.generation() == 3U);
    assert(document.object(0U)->category_index == 4U);

    AnnotationColorRange sup;
    sup.center.hue_degrees = 32.0f;
    sup.tolerance.hue_plus_pct = 8.0f;
    sup.sampling = false;
    AnnotationColorRange nosup;
    nosup.center.hue_degrees = 180.0f;
    nosup.tolerance.value_minus_pct = 5.0f;
    nosup.sampling = true;
    assert(document.apply(AnnotationSetObjectColorRangesCommand{
        0U,
        sup,
        nosup,
    }));
    assert(document.generation() == 4U);
    assert(document.object(0U) != nullptr);
    assert(document.object(0U)->sup.center.hue_degrees == 32.0f);
    assert(document.object(0U)->nosup.sampling);
}

void test_tool_manager_resets_outgoing_tool_on_switch() {
    AnnotationToolManager manager;
    AnnotationDocument document;
    int select_reset_count = 0;
    int box_reset_count = 0;
    manager.register_tool(std::make_unique<TrackingAnnotationTool>(AnnotationToolKind::Select, &select_reset_count));
    manager.register_tool(std::make_unique<TrackingAnnotationTool>(AnnotationToolKind::Box, &box_reset_count));

    AnnotationSession session;
    assert(session.active_tool() == AnnotationToolKind::Select);

    assert(manager.set_active_tool(AnnotationToolKind::Box, document, session));
    assert(session.active_tool() == AnnotationToolKind::Box);
    assert(select_reset_count == 1);
    assert(box_reset_count == 0);
}

void test_renderer_builds_interaction_overlay_snapshot() {
    AnnotationInteractionOverlayRequest request;
    request.width = 640U;
    request.height = 480U;
    request.cuda_device_index = 2;
    request.crop_box = AnnotationBox{10, 20, 110, 120};
    request.crop_handle_radius = 9;
    request.drag_box = AnnotationBox{40, 50, 140, 150};
    request.create_box = AnnotationBox{200, 210, 260, 280};
    request.objects = {
        AnnotationInteractionOverlayObject{0U, 0U, AnnotationShapeType::Box, AnnotationBox{12, 18, 60, 90}, false,
                                           false},
        AnnotationInteractionOverlayObject{1U, 1U, AnnotationShapeType::Box, AnnotationBox{80, 40, 120, 100}, true,
                                           false},
        AnnotationInteractionOverlayObject{2U, 2U, AnnotationShapeType::Box, AnnotationBox{130, 60, 180, 140}, false,
                                           true},
    };
    request.polylines = {
        PreviewInteractionOverlayPolyline{
            {
                PreviewInteractionOverlayPoint{16, 24},
                PreviewInteractionOverlayPoint{28, 36},
                PreviewInteractionOverlayPoint{40, 30},
            },
            false,
            240U,
            196U,
            68U,
            2,
        },
    };
    request.marker_sets = {
        PreviewInteractionOverlayMarkerSet{
            {
                PreviewInteractionOverlayPoint{18, 22},
                PreviewInteractionOverlayPoint{44, 52},
            },
            4,
            255U,
            220U,
            96U,
            240U,
        },
    };
    request.skeletons = {
        PreviewInteractionOverlaySkeleton{
            {
                PreviewInteractionOverlayPoint{70, 76},
                PreviewInteractionOverlayPoint{96, 102},
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

    const PreviewInteractionOverlaySnapshot snapshot = AnnotationRenderer::build_interaction_overlay_snapshot(request);
    assert(snapshot.width == 640U);
    assert(snapshot.height == 480U);
    assert(snapshot.cuda_device_index == 2);
    assert(snapshot.boxes.size() == 6U);
    assert(snapshot.polylines.size() == 1U);
    assert(snapshot.marker_sets.size() == 1U);
    assert(snapshot.skeletons.size() == 1U);
    assert(snapshot.boxes[0].draw_handles);
    assert(snapshot.boxes[0].handle_radius == 9);
    assert(snapshot.boxes[4].draw_handles);
    assert(!snapshot.boxes[5].draw_handles);
}

void test_annotation_workspace_model_projects_selection_and_crop() {
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    session.select_object(0U);

    AnnotationFrame frame;
    frame.width = 96U;
    frame.height = 72U;
    frame.capture_width = 96U;
    frame.capture_height = 72U;

    SourceSelectionState source;
    source.kind = SourceKind::VideoStream;
    source.capture_width = 96;
    source.capture_height = 72;
    source.crop_x = 8;
    source.crop_y = 10;
    source.crop_width = 32;
    source.crop_height = 24;

    const AnnotationWorkspaceViewModel model =
        AnnotationWorkspaceModelBuilder::build(frame, document, session, source, true);
    const auto selected_object_index = require_optional_value(model.selection.selected_object_index,
                                                              "expected selected object index in workspace view model");
    assert(selected_object_index == 0U);
    const AnnotationBox& selected_capture_box = require_optional_ref(
        model.selection.selected_capture_box, "expected selected capture box in workspace view model");
    assert(selected_capture_box.x1 == 4);
    assert(selected_capture_box.y1 == 5);
    assert(model.selection.selected_frame_box.has_value());
    assert(model.selection.selected_box_fully_visible);
    assert(model.projected_scene != nullptr);
    assert(model.projected_scene->visible_objects.size() == 1U);
    const AnnotationBox& crop_frame_box =
        require_optional_ref(model.crop_frame_box, "expected crop frame box in workspace view model");
    assert(crop_frame_box.x1 == 8);
    assert(crop_frame_box.y1 == 10);
    assert(crop_frame_box.x2 == 40);
    assert(crop_frame_box.y2 == 34);
}

void test_annotation_document_transform_commands_update_generation() {
    AnnotationDocument document;
    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    assert(document.generation() == 1U);

    assert(document.apply(AnnotationTranslateObjectCommand{
        0U,
        9,
        11,
        128U,
        128U,
    }));
    assert(document.generation() == 2U);
    const AnnotationObject* translated = document.object(0U);
    assert(translated != nullptr);
    const AnnotationBoxShape* translated_box = std::get_if<AnnotationBoxShape>(&translated->shape);
    assert(translated_box != nullptr);
    assert(translated_box->box.x1 == 13);
    assert(translated_box->box.y1 == 16);
    assert(translated_box->box.x2 == 21);
    assert(translated_box->box.y2 == 29);

    assert(document.apply(AnnotationResizeObjectCommand{
        0U,
        AnnotationBox{20, 24, 42, 56},
        128U,
        128U,
    }));
    assert(document.generation() == 3U);
    const AnnotationObject* resized = document.object(0U);
    assert(resized != nullptr);
    const AnnotationBoxShape* resized_box = std::get_if<AnnotationBoxShape>(&resized->shape);
    assert(resized_box != nullptr);
    assert(resized_box->box.x1 == 20);
    assert(resized_box->box.y1 == 24);
    assert(resized_box->box.x2 == 42);
    assert(resized_box->box.y2 == 56);
}

void test_renderer_manual_overlay_snapshot_tracks_document_and_session_revisions() {
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    session.select_object(0U);
    session.set_active_tool(AnnotationToolKind::MaskPaint);

    const AnnotationFrame frame = make_annotation_frame(64U, 48U);

    const mmltk::live::ManualOverlayDocumentSnapshot snapshot =
        AnnotationRenderer::build_manual_overlay_snapshot(frame, document, session);
    assert(snapshot.document_generation == document.generation());
    assert(snapshot.session_revision == session.overlay_revision());
    assert(snapshot.capture_width == 64U);
    assert(snapshot.capture_height == 48U);
    const auto selected_instance =
        require_optional_value(snapshot.selected_instance, "expected selected instance in manual overlay snapshot");
    assert(selected_instance == 0U);
    assert(snapshot.instances.size() == 1U);
}

void test_renderer_manual_overlay_snapshot_includes_vector_primitives() {
    AnnotationDocument document;

    AnnotationObject spline_object = make_smooth_two_knot_spline_annotation_object("spline-1", 0U, true);
    assert(document.apply(AnnotationInsertObjectCommand{spline_object, std::nullopt}));

    AnnotationObject skeleton_object;
    skeleton_object.object_id = "skeleton-1";
    skeleton_object.category_index = 0U;
    skeleton_object.shape = AnnotationSkeletonShape{
        {
            AnnotationSkeletonNode{"left", AnnotationPoint{40.0f, 22.0f}, true},
            AnnotationSkeletonNode{"right", AnnotationPoint{52.0f, 24.0f}, true},
            AnnotationSkeletonNode{"hidden", AnnotationPoint{48.0f, 40.0f}, false},
        },
        {
            AnnotationSkeletonEdge{0U, 1U},
            AnnotationSkeletonEdge{1U, 2U},
        },
    };
    assert(document.apply(AnnotationInsertObjectCommand{skeleton_object, std::nullopt}));

    const AnnotationFrame frame = make_render_frame();

    const mmltk::live::ManualOverlayDocumentSnapshot snapshot =
        AnnotationRenderer::build_manual_overlay_snapshot(frame, document, AnnotationSession{});
    assert(snapshot.instances.size() == 2U);

    const auto& spline_overlay = snapshot.instances[0];
    assert(spline_overlay.polyline_closed);
    assert(spline_overlay.polyline_points.size() > 2U);
    assert(spline_overlay.points.size() == 2U);

    const auto& skeleton_overlay = snapshot.instances[1];
    assert(skeleton_overlay.points.size() == 2U);
    assert(skeleton_overlay.skeleton_edges.size() == 1U);
    assert(skeleton_overlay.skeleton_edges[0].source_index == 0U);
    assert(skeleton_overlay.skeleton_edges[0].target_index == 1U);
}

void test_renderer_builds_editable_handles_for_selected_spline() {
    AnnotationDocument document;

    AnnotationObject spline_object = make_smooth_two_knot_spline_annotation_object("spline-1", 2U, false);
    assert(document.apply(AnnotationInsertObjectCommand{spline_object, std::nullopt}));

    const std::vector<AnnotationEditableHandle> handles = build_selected_editable_handles(document);
    assert(handles.size() == 6U);
    assert(handles[0].id.role == AnnotationHandleRole::SplineKnot);
    assert(handles[0].id.element_index == 0U);
    assert(handles[1].id.role == AnnotationHandleRole::SplineInHandle);
    assert(handles[1].id.element_index == 0U);
    assert(!handles[1].materialized);
    assert(handles[2].id.role == AnnotationHandleRole::SplineOutHandle);
    assert(handles[2].id.element_index == 0U);
    assert(handles[2].materialized);
    assert(handles[3].id.role == AnnotationHandleRole::SplineKnot);
    assert(handles[3].id.element_index == 1U);
    assert(handles[4].id.role == AnnotationHandleRole::SplineInHandle);
    assert(handles[4].id.element_index == 1U);
    assert(handles[4].materialized);
    assert(handles[5].id.role == AnnotationHandleRole::SplineOutHandle);
    assert(handles[5].id.element_index == 1U);
    assert(!handles[5].materialized);
    assert(handles[2].frame_point.x == 18.0f);
    assert(handles[4].frame_point.y == 18.0f);
}

void test_renderer_exposes_latent_handles_for_selected_spline() {
    AnnotationDocument document;

    AnnotationObject spline_object;
    spline_object.object_id = "spline-1";
    spline_object.category_index = 1U;
    spline_object.shape = AnnotationSplineShape{
        false,
        {
            AnnotationSplineKnot{
                AnnotationPoint{10.0f, 12.0f},
                {},
                {},
                AnnotationSplineHandleMode::Corner,
            },
            AnnotationSplineKnot{
                AnnotationPoint{24.0f, 18.0f},
                {},
                {},
                AnnotationSplineHandleMode::Corner,
            },
            AnnotationSplineKnot{
                AnnotationPoint{40.0f, 20.0f},
                {},
                {},
                AnnotationSplineHandleMode::Corner,
            },
        },
    };
    assert(document.apply(AnnotationInsertObjectCommand{spline_object, std::nullopt}));

    const std::vector<AnnotationEditableHandle> handles = build_selected_editable_handles(document);
    assert(handles.size() == 7U);

    std::size_t latent_handle_count = 0U;
    for (const AnnotationEditableHandle& handle : handles) {
        if (!handle.materialized) {
            ++latent_handle_count;
            assert(handle.has_tether);
        }
    }
    assert(latent_handle_count == 4U);
}

void test_remap_skeleton_annotation_object_to_category_preserves_named_nodes() {
    AnnotationObject object;
    object.object_id = "skeleton-1";
    object.category_index = 0U;
    object.shape = AnnotationSkeletonShape{
        {
            AnnotationSkeletonNode{"head", AnnotationPoint{8.0f, 12.0f}, true},
            AnnotationSkeletonNode{"tail", AnnotationPoint{32.0f, 28.0f}, true},
        },
        {
            AnnotationSkeletonEdge{0U, 1U},
        },
    };

    AnnotationCategory category;
    category.id = 2;
    category.name = "animal";
    category.keypoints = {"tail", "left", "head"};
    category.skeleton_edges = {
        AnnotationCategorySkeletonEdge{0U, 1U},
        AnnotationCategorySkeletonEdge{1U, 2U},
    };

    assert(remap_skeleton_annotation_object_to_category(&object, &category));
    const AnnotationSkeletonShape* skeleton = std::get_if<AnnotationSkeletonShape>(&object.shape);
    assert(skeleton != nullptr);
    assert(skeleton->nodes.size() == 3U);
    assert(skeleton->edges.size() == 2U);
    assert(skeleton->nodes[0].key == "tail");
    assert(skeleton->nodes[0].visible);
    assert(skeleton->nodes[0].point.x == 32.0f);
    assert(skeleton->nodes[1].key == "left");
    assert(!skeleton->nodes[1].visible);
    assert(skeleton->nodes[2].key == "head");
    assert(skeleton->nodes[2].visible);
    assert(skeleton->nodes[2].point.y == 12.0f);
}

AnnotationFrame make_controller_frame() {
    AnnotationFrame frame;
    frame.width = 96U;
    frame.height = 72U;
    frame.capture_width = 96U;
    frame.capture_height = 72U;
    return frame;
}

std::optional<std::size_t> create_skeleton_object(AnnotationController& controller, AnnotationDocument& document,
                                                  AnnotationSession& session, const AnnotationFrame& frame,
                                                  AnnotationCategories& categories, const std::size_t category_index) {
    return controller.create_object(AnnotationToolKind::Skeleton, document, session, frame, categories, category_index);
}

AnnotationVisibleObjectHit hit_test_single_object(const AnnotationObject& object, const CanvasViewport& viewport,
                                                  const CanvasPointerState pointer, const int capture_x,
                                                  const int capture_y, const std::string_view message) {
    AnnotationDocument document;
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    const AnnotationProjectedScene scene =
        AnnotationRenderer::build_projected_scene(make_controller_frame(), document, std::nullopt);
    const std::optional<AnnotationVisibleObjectHit> hit = AnnotationRenderer::hit_test_visible_objects(
        scene.visible_objects, viewport, pointer, capture_x, capture_y, false);
    return require_optional_ref(hit, message);
}

struct ObjectsTabApplyFixture {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    AnnotationFrame frame = make_controller_frame();

    [[nodiscard]] AnnotationObjectsTabApplyContext make_context(const AnnotationFrame* frame_override) {
        return make_objects_tab_apply_context(controller, document, session, categories, frame_override);
    }

    [[nodiscard]] AnnotationSidebarMutationResult apply_selected_edit(
        const AnnotationSelectedObjectSidebarActionPayload& request, const AnnotationFrame* frame_override) {
        return apply_annotation_selected_object_sidebar_payload(make_context(frame_override), request);
    }
};

void test_annotation_controller_create_object_seeds_spline_grouped_edit_state() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    const AnnotationFrame frame = make_controller_frame();

    const std::optional<std::size_t> created_index =
        controller.create_object(AnnotationToolKind::Spline, document, session, frame, categories, 0U);
    const auto created_index_value = require_optional_value(created_index, "expected created spline object index");
    assert(created_index_value == 0U);
    assert(document.transaction_active());
    const auto selected_object_index_value =
        require_optional_value(session.selected_object_index(), "expected selected object index after spline creation");
    assert(selected_object_index_value == 0U);
    const auto& grouped_edit_transaction = session.grouped_edit_transaction();
    assert(grouped_edit_transaction.kind == AnnotationEditTransactionKind::SplineConstruction);
    const auto grouped_object_index =
        require_optional_value(grouped_edit_transaction.object_index, "expected grouped spline edit object index");
    assert(grouped_object_index == 0U);
    const auto& spline_edit_state = session.spline_edit_state();
    const auto spline_object_index =
        require_optional_value(spline_edit_state.object_index, "expected spline edit state object index");
    assert(spline_object_index == 0U);
    assert(!spline_edit_state.active_knot_index.has_value());
    assert(!spline_edit_state.active_segment_index.has_value());
}

void test_annotation_controller_create_object_commits_empty_skeleton_grouped_edit() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    const AnnotationFrame frame = make_controller_frame();

    const std::optional<std::size_t> created_index =
        create_skeleton_object(controller, document, session, frame, categories, 0U);
    const auto created_index_value = require_optional_value(created_index, "expected created skeleton object index");
    assert(created_index_value == 0U);
    assert(!document.transaction_active());
    assert(session.grouped_edit_transaction().kind == AnnotationEditTransactionKind::None);
    const auto& skeleton_edit_state = session.skeleton_edit_state();
    const auto skeleton_object_index =
        require_optional_value(skeleton_edit_state.object_index, "expected skeleton edit state object index");
    assert(skeleton_object_index == 0U);
    assert(!skeleton_edit_state.active_joint_index.has_value());
}

void test_annotation_controller_create_object_keeps_skeleton_grouped_edit_open_for_topology() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    const std::size_t category_index = ensure_annotation_category(categories, "pose");
    categories.items[category_index].keypoints = {"hip", "knee"};
    categories.items[category_index].skeleton_edges = {
        AnnotationCategorySkeletonEdge{0U, 1U},
    };
    const AnnotationFrame frame = make_controller_frame();

    const std::optional<std::size_t> created_index =
        create_skeleton_object(controller, document, session, frame, categories, category_index);
    const auto created_index_value =
        require_optional_value(created_index, "expected created skeleton topology object index");
    assert(created_index_value == 0U);
    assert(document.transaction_active());
    const auto& grouped_edit_transaction = session.grouped_edit_transaction();
    assert(grouped_edit_transaction.kind == AnnotationEditTransactionKind::SkeletonConstruction);
    const auto grouped_object_index =
        require_optional_value(grouped_edit_transaction.object_index, "expected grouped skeleton edit object index");
    assert(grouped_object_index == 0U);
    const auto& skeleton_edit_state = session.skeleton_edit_state();
    const auto skeleton_object_index =
        require_optional_value(skeleton_edit_state.object_index, "expected skeleton edit state object index");
    assert(skeleton_object_index == 0U);
    const auto active_joint_index =
        require_optional_value(skeleton_edit_state.active_joint_index, "expected active skeleton joint index");
    assert(active_joint_index == 0U);
}

void test_projected_scene_cache_refreshes_selection_without_invalidating_layout() {
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationObject first = make_test_annotation_object();
    AnnotationObject second = first;
    second.object_id = "manual-2";
    second.shape = AnnotationBoxShape{AnnotationBox{30, 10, 48, 26}};
    assert(document.apply(AnnotationInsertObjectCommand{first, std::nullopt}));
    assert(document.apply(AnnotationInsertObjectCommand{second, std::nullopt}));

    AnnotationFrame frame = make_controller_frame();
    AnnotationProjectedSceneCache cache;

    session.select_object(0U);
    const std::shared_ptr<const AnnotationProjectedScene> first_scene =
        cache.resolve(AnnotationProjectedSceneCacheInputs{
            &document,
            &session,
            &frame,
        });
    assert(first_scene != nullptr);
    const auto first_selected_object_index =
        require_optional_value(first_scene->selected_object_index, "expected first projected scene selection");
    assert(first_selected_object_index == 0U);

    session.select_object(1U);
    const std::shared_ptr<const AnnotationProjectedScene> second_scene =
        cache.resolve(AnnotationProjectedSceneCacheInputs{
            &document,
            &session,
            &frame,
        });
    assert(second_scene != nullptr);
    assert(second_scene != first_scene);
    const auto second_selected_object_index =
        require_optional_value(second_scene->selected_object_index, "expected second projected scene selection");
    assert(second_selected_object_index == 1U);
    assert(second_scene->visible_objects.size() == first_scene->visible_objects.size());
    assert(second_scene->editable_handles.size() == first_scene->editable_handles.size());
}

void test_annotation_editor_undo_redo_reselects_object_by_id_after_reorder() {
    AnnotationDocument document;
    AnnotationSession session;

    AnnotationObject first = make_test_annotation_object();
    first.object_id = "box-1";
    assert(document.apply(AnnotationInsertObjectCommand{first, std::nullopt}));

    AnnotationObject second = make_test_annotation_object();
    second.object_id = "box-2";
    second.shape = AnnotationBoxShape{AnnotationBox{20, 22, 32, 38}};
    assert(document.apply(AnnotationInsertObjectCommand{second, std::nullopt}));

    session.select_object(1U);

    AnnotationObject inserted = make_test_annotation_object();
    inserted.object_id = "box-0";
    inserted.shape = AnnotationBoxShape{AnnotationBox{1, 2, 6, 8}};
    assert(AnnotationEditor::insert_object(document, session, inserted, 0U));
    const auto selected_after_insert =
        require_optional_value(session.selected_object_index(), "expected selected object index after insert");
    assert(selected_after_insert == 2U);
    assert(require_pointer(document.object(selected_after_insert), "expected selected object after insert").object_id ==
           "box-2");

    assert(AnnotationEditor::undo(document, session));
    const auto selected_after_undo =
        require_optional_value(session.selected_object_index(), "expected selected object index after undo");
    assert(selected_after_undo == 1U);
    assert(require_pointer(document.object(selected_after_undo), "expected selected object after undo").object_id ==
           "box-2");

    assert(AnnotationEditor::redo(document, session));
    const auto selected_after_redo =
        require_optional_value(session.selected_object_index(), "expected selected object index after redo");
    assert(selected_after_redo == 2U);
    assert(require_pointer(document.object(selected_after_redo), "expected selected object after redo").object_id ==
           "box-2");
}

void test_cancel_grouped_edit_for_selection_restores_prior_selection_and_cancels_changes() {
    AnnotationDocument document;
    AnnotationSession session;

    AnnotationObject box = make_test_annotation_object();
    box.object_id = "box-1";
    assert(document.apply(AnnotationInsertObjectCommand{box, std::nullopt}));

    AnnotationObject spline;
    spline.object_id = "spline-1";
    spline.category_index = 0U;
    spline.shape = AnnotationSplineShape{
        false,
        {
            AnnotationSplineKnot{
                AnnotationPoint{12.0f, 14.0f},
                AnnotationSplineHandle{},
                AnnotationSplineHandle{},
                AnnotationSplineHandleMode::Corner,
            },
        },
    };
    assert(document.apply(AnnotationInsertObjectCommand{spline, std::nullopt}));

    session.select_object(1U);
    session.set_spline_edit_state(AnnotationSplineEditState{
        1U,
        0U,
        std::nullopt,
        false,
        false,
    });
    assert(begin_grouped_edit(document, session, AnnotationEditTransactionKind::SplineConstruction, 1U, 0U));
    assert(document.transaction_active());
    assert(document.apply(AnnotationAppendSplineKnotCommand{
        1U,
        AnnotationPoint{20.0f, 24.0f},
    }));

    const AnnotationObject* active_object = document.object(1U);
    assert(active_object != nullptr);
    const AnnotationSplineShape* active_spline = std::get_if<AnnotationSplineShape>(&active_object->shape);
    assert(active_spline != nullptr);
    assert(active_spline->knots.size() == 2U);

    select_object(session, document, 0U);
    assert(!document.transaction_active());
    const auto selected_object_index =
        require_optional_value(session.selected_object_index(), "expected selected object index after selection");
    assert(selected_object_index == 0U);
    assert(session.grouped_edit_transaction().kind == AnnotationEditTransactionKind::None);
    assert(!session.spline_edit_state().object_index.has_value());

    active_object = document.object(1U);
    assert(active_object != nullptr);
    active_spline = std::get_if<AnnotationSplineShape>(&active_object->shape);
    assert(active_spline != nullptr);
    assert(active_spline->knots.size() == 1U);
}

void test_sidebar_semantic_edits_commit_as_single_undo_step() {
    ObjectsTabApplyFixture fixture;
    ensure_annotation_category(fixture.categories, "box");
    ensure_annotation_category(fixture.categories, "alt");

    AnnotationObject object = make_test_annotation_object();
    object.object_id = "box-1";
    assert(fixture.document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    fixture.session.select_object(0U);

    AnnotationSelectedObjectSidebarActionPayload request;
    request.selected_object_index = 0U;
    request.update_selected_metadata = true;
    request.selected_enabled = false;
    request.selected_category_index = 1U;
    request.request_redraw_box = true;

    const AnnotationSidebarMutationResult result = fixture.apply_selected_edit(request, &fixture.frame);
    assert(result.preview_invalidated);
    assert(result.reset_canvas_interactions);
    const auto next_tool = require_optional_value(result.next_tool, "expected next tool after sidebar mutation");
    assert(next_tool == AnnotationToolKind::Box);

    const AnnotationObject* edited = fixture.document.object(0U);
    assert(edited != nullptr);
    assert(!edited->enabled);
    assert(edited->category_index == 1U);
    const AnnotationBoxShape* edited_box = std::get_if<AnnotationBoxShape>(&edited->shape);
    assert(edited_box != nullptr);
    assert(!annotation_box_has_area(edited_box->box));

    assert(AnnotationEditor::undo(fixture.document, fixture.session));
    const AnnotationObject* restored = fixture.document.object(0U);
    assert(restored != nullptr);
    assert(restored->enabled);
    assert(restored->category_index == 0U);
    const AnnotationBoxShape* restored_box = std::get_if<AnnotationBoxShape>(&restored->shape);
    assert(restored_box != nullptr);
    assert(restored_box->box.x1 == 4);
    assert(restored_box->box.y1 == 5);
    assert(restored_box->box.x2 == 12);
    assert(restored_box->box.y2 == 18);
}

void test_sidebar_mutation_effects_gate_assist_and_apply_follow_up_actions() {
    AnnotationSidebarMutationResult result;
    result.preview_invalidated = true;
    result.reset_canvas_interactions = true;
    result.next_tool = AnnotationToolKind::Skeleton;
    result.request_assist = true;

    int assist_count = 0;
    int reset_count = 0;
    int invalidate_count = 0;
    std::optional<AnnotationToolKind> selected_tool;

    apply_annotation_sidebar_mutation_effects(
        result, false, [&assist_count]() { ++assist_count; }, [&reset_count]() { ++reset_count; },
        [&selected_tool](const AnnotationToolKind tool) { selected_tool = tool; },
        [&invalidate_count]() { ++invalidate_count; });
    assert(assist_count == 0);
    assert(reset_count == 1);
    assert(selected_tool == AnnotationToolKind::Skeleton);
    assert(invalidate_count == 1);

    selected_tool.reset();
    apply_annotation_sidebar_mutation_effects(
        result, true, [&assist_count]() { ++assist_count; }, [&reset_count]() { ++reset_count; },
        [&selected_tool](const AnnotationToolKind tool) { selected_tool = tool; },
        [&invalidate_count]() { ++invalidate_count; });
    assert(assist_count == 1);
    assert(reset_count == 2);
    assert(selected_tool == AnnotationToolKind::Skeleton);
    assert(invalidate_count == 2);
}

void test_mask_sidebar_edits_commit_cleanup_and_color_ranges_in_one_undo_step() {
    AnnotationDocument document;
    AnnotationSession session;
    const AnnotationFrame frame = make_controller_frame();
    AnnotationObject object = make_test_mask_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    session.select_object(0U);

    int cleanup_radius = 1;
    AnnotationMaskSidebarActionPayload request;
    request.cleanup_radius = 5;
    request.cleanup_op = AnnotationMaskCleanupOp::FillHoles;
    request.update_color_ranges = true;
    request.sup.center.hue_degrees = 120.0f;
    request.sup.tolerance.hue_plus_pct = 7.0f;
    request.nosup.center.hue_degrees = 240.0f;
    request.nosup.tolerance.value_minus_pct = 12.0f;
    request.nosup.sampling = true;

    const AnnotationSidebarMutationResult result = apply_annotation_mask_sidebar_edit(
        AnnotationMaskTabApplyContext{
            document,
            session,
            &frame,
            &cleanup_radius,
        },
        request);
    assert(result.preview_invalidated);
    assert(cleanup_radius == 5);

    const AnnotationObject& edited = require_pointer(document.object(0U), "expected edited mask object");
    const AnnotationMaskShape& edited_mask =
        require_pointer(annotation_object_mask_shape(edited), "expected selected object mask after sidebar edits");
    assert(edited_mask.mask.size() == 9U);
    assert(edited_mask.mask[4] == 1U);
    assert(edited.sup.center.hue_degrees == 120.0f);
    assert(edited.sup.tolerance.hue_plus_pct == 7.0f);
    assert(edited.nosup.center.hue_degrees == 240.0f);
    assert(edited.nosup.tolerance.value_minus_pct == 12.0f);
    assert(edited.nosup.sampling);

    assert(AnnotationEditor::undo(document, session));
    const AnnotationObject& restored = require_pointer(document.object(0U), "expected restored mask object");
    const AnnotationMaskShape& restored_mask =
        require_pointer(annotation_object_mask_shape(restored), "expected restored selected object mask");
    assert(restored_mask.mask.size() == 9U);
    assert(restored_mask.mask[4] == 0U);
    assert(restored.sup.center.hue_degrees == 16.0f);
    assert(restored.nosup.center.hue_degrees == 48.0f);
    assert(!restored.nosup.sampling);
}

void test_spline_sidebar_edits_insert_knot_and_update_handle_mode() {
    ObjectsTabApplyFixture fixture;
    AnnotationObject object = make_test_spline_annotation_object();
    assert(fixture.document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    fixture.session.select_object(0U);

    AnnotationSelectedObjectSidebarActionPayload request;
    request.update_spline_active_segment = true;
    request.spline_active_segment_index = 0U;
    request.request_insert_active_spline_knot = true;
    request.spline_handle_mode = AnnotationSplineHandleMode::Mirrored;

    const AnnotationSidebarMutationResult result = fixture.apply_selected_edit(request, nullptr);
    assert(result.preview_invalidated);
    assert(result.next_tool == AnnotationToolKind::Spline);

    const AnnotationObject& edited = require_pointer(fixture.document.object(0U), "expected edited spline object");
    const AnnotationSplineShape* spline = std::get_if<AnnotationSplineShape>(&edited.shape);
    assert(spline != nullptr);
    assert(spline->knots.size() == 3U);
    assert(spline->knots[1].handle_mode == AnnotationSplineHandleMode::Mirrored);

    const auto active_knot_index = require_optional_value(fixture.session.spline_edit_state().active_knot_index,
                                                          "expected active spline knot after sidebar insert");
    assert(active_knot_index == 1U);
}

void test_skeleton_sidebar_edits_update_active_joint_and_hide_selected_joint() {
    ObjectsTabApplyFixture fixture;
    AnnotationObject object = make_test_skeleton_annotation_object();
    assert(fixture.document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    fixture.session.select_object(0U);

    AnnotationSelectedObjectSidebarActionPayload request;
    request.update_skeleton_active_joint = true;
    request.skeleton_active_joint_index = 1U;
    request.skeleton_action = AnnotationToolActionKind::HideJoint;

    const AnnotationSidebarMutationResult result = fixture.apply_selected_edit(request, &fixture.frame);
    assert(result.preview_invalidated);
    assert(result.next_tool == AnnotationToolKind::Skeleton);

    const AnnotationObject& edited = require_pointer(fixture.document.object(0U), "expected edited skeleton object");
    const AnnotationSkeletonShape* skeleton = std::get_if<AnnotationSkeletonShape>(&edited.shape);
    assert(skeleton != nullptr);
    assert(skeleton->nodes.size() == 2U);
    assert(!skeleton->nodes[1].visible);

    const auto active_joint_index = require_optional_value(fixture.session.skeleton_edit_state().active_joint_index,
                                                           "expected active skeleton joint after sidebar update");
    assert(active_joint_index == 1U);
}

void test_renderer_hit_tests_non_box_geometry() {
    const CanvasViewport viewport = make_canvas_viewport(0.0f, 0.0f, 96.0f, 72.0f, 96U, 72U);

    {
        AnnotationObject point;
        point.object_id = "point-1";
        point.shape = AnnotationPointShape{AnnotationPoint{24.0f, 28.0f}};
        const AnnotationVisibleObjectHit& hit_value = hit_test_single_object(
            point, viewport, CanvasPointerState{24.0f, 28.0f, true, false, false}, 24, 28, "expected point hit");
        assert(hit_value.object.shape_type == AnnotationShapeType::Point);
    }

    {
        AnnotationObject spline;
        spline.object_id = "spline-1";
        spline.shape = AnnotationSplineShape{
            false,
            {
                AnnotationSplineKnot{
                    AnnotationPoint{12.0f, 12.0f},
                    AnnotationSplineHandle{},
                    AnnotationSplineHandle{},
                    AnnotationSplineHandleMode::Corner,
                },
                AnnotationSplineKnot{
                    AnnotationPoint{36.0f, 36.0f},
                    AnnotationSplineHandle{},
                    AnnotationSplineHandle{},
                    AnnotationSplineHandleMode::Corner,
                },
            },
        };
        const AnnotationVisibleObjectHit& hit_value = hit_test_single_object(
            spline, viewport, CanvasPointerState{24.0f, 24.0f, true, false, false}, 24, 24, "expected spline hit");
        assert(hit_value.object.shape_type == AnnotationShapeType::Spline);
    }

    {
        AnnotationObject skeleton;
        skeleton.object_id = "skeleton-1";
        skeleton.shape = AnnotationSkeletonShape{
            {
                AnnotationSkeletonNode{"a", AnnotationPoint{56.0f, 16.0f}, true},
                AnnotationSkeletonNode{"b", AnnotationPoint{72.0f, 32.0f}, true},
            },
            {
                AnnotationSkeletonEdge{0U, 1U},
            },
        };
        const AnnotationVisibleObjectHit& hit_value = hit_test_single_object(
            skeleton, viewport, CanvasPointerState{64.0f, 24.0f, true, false, false}, 64, 24, "expected skeleton hit");
        assert(hit_value.object.shape_type == AnnotationShapeType::Skeleton);
    }
}

void test_annotation_controller_canvas_click_creates_and_updates_point_objects() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    ensure_annotation_category(categories, "point");

    assert(controller.set_active_tool(AnnotationToolKind::Point, document, session));
    assert(controller.handle_canvas_click(document, session, make_controller_frame(), categories, 24, 28, false));
    assert(document.size() == 1U);
    assert(session.selected_object_index().has_value());
    const AnnotationObject* created = document.object(0U);
    assert(created != nullptr);
    const auto* point_shape = std::get_if<AnnotationPointShape>(&created->shape);
    assert(point_shape != nullptr);
    assert(point_shape->point.x == 24.0f);
    assert(point_shape->point.y == 28.0f);

    assert(controller.handle_canvas_click(document, session, make_controller_frame(), categories, 48, 52, false));
    created = document.object(0U);
    assert(created != nullptr);
    point_shape = std::get_if<AnnotationPointShape>(&created->shape);
    assert(point_shape != nullptr);
    assert(point_shape->point.x == 48.0f);
    assert(point_shape->point.y == 52.0f);
}

void test_annotation_controller_box_commit_updates_box_and_mask_tools() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    ensure_annotation_category(categories, "box");
    AnnotationFrame frame = make_controller_frame();

    assert(controller.create_object(AnnotationToolKind::Box, document, session, frame, categories, 0U).has_value());
    assert(controller.set_active_tool(AnnotationToolKind::Box, document, session));
    assert(controller.handle_box_commit(document, session, frame, categories, RectDragKind::Create,
                                        AnnotationBox{8, 10, 24, 32}, 0U));
    const AnnotationObject* boxed = document.object(0U);
    assert(boxed != nullptr);
    const auto* box_shape = std::get_if<AnnotationBoxShape>(&boxed->shape);
    assert(box_shape != nullptr);
    assert(box_shape->box.x1 == 8);
    assert(box_shape->box.y1 == 10);
    assert(box_shape->box.x2 == 24);
    assert(box_shape->box.y2 == 32);

    assert(controller.set_active_tool(AnnotationToolKind::MaskPaint, document, session));
    assert(controller.handle_brush_sample(document, session, frame, categories, 16, 20, 3));
    boxed = document.object(0U);
    assert(boxed != nullptr);
    const AnnotationMaskShape* mask_shape = annotation_object_mask_shape(*boxed);
    assert(mask_shape != nullptr);
    assert(!mask_shape->mask.empty());
}

void test_annotation_controller_direct_tool_moves_selected_box() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    ensure_annotation_category(categories, "box");
    AnnotationFrame frame = make_controller_frame();

    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    session.select_object(0U);
    assert(controller.set_active_tool(AnnotationToolKind::Direct, document, session));
    session.set_direct_drag_index(0U);
    assert(controller.handle_box_commit(document, session, frame, categories, RectDragKind::Move,
                                        AnnotationBox{20, 24, 28, 37}, 0U));

    const AnnotationObject* moved = document.object(0U);
    assert(moved != nullptr);
    const auto* box_shape = std::get_if<AnnotationBoxShape>(&moved->shape);
    assert(box_shape != nullptr);
    assert(box_shape->box.x1 == 20);
    assert(box_shape->box.y1 == 24);
    assert(box_shape->box.x2 == 28);
    assert(box_shape->box.y2 == 37);
}

void test_annotation_document_handle_commands_update_generation() {
    AnnotationDocument document;

    AnnotationObject spline_object;
    spline_object.object_id = "spline-1";
    spline_object.category_index = 0U;
    spline_object.shape = AnnotationSplineShape{
        false,
        {
            AnnotationSplineKnot{
                AnnotationPoint{10.0f, 10.0f},
                AnnotationSplineHandle{AnnotationPoint{6.0f, 10.0f}, true},
                AnnotationSplineHandle{AnnotationPoint{14.0f, 10.0f}, true},
                AnnotationSplineHandleMode::Mirrored,
            },
        },
    };
    assert(document.apply(AnnotationInsertObjectCommand{spline_object, std::nullopt}));
    assert(document.generation() == 1U);

    assert(document.apply(AnnotationSetHandlePositionCommand{
        AnnotationHandleId{0U, 0U, AnnotationHandleRole::SplineOutHandle},
        AnnotationPoint{18.0f, 12.0f},
        64U,
        64U,
    }));
    assert(document.generation() == 2U);
    const AnnotationObject* updated = document.object(0U);
    assert(updated != nullptr);
    const auto* spline = std::get_if<AnnotationSplineShape>(&updated->shape);
    assert(spline != nullptr);
    assert(spline->knots[0].out_handle.enabled);
    assert(spline->knots[0].out_handle.position.x == 18.0f);
    assert(spline->knots[0].out_handle.position.y == 12.0f);
    assert(spline->knots[0].in_handle.enabled);
    assert(spline->knots[0].in_handle.position.x == 2.0f);
    assert(spline->knots[0].in_handle.position.y == 8.0f);

    assert(document.apply(AnnotationSetHandlePositionCommand{
        AnnotationHandleId{0U, 0U, AnnotationHandleRole::SplineKnot},
        AnnotationPoint{20.0f, 20.0f},
        64U,
        64U,
    }));
    assert(document.generation() == 3U);
    updated = document.object(0U);
    assert(updated != nullptr);
    spline = std::get_if<AnnotationSplineShape>(&updated->shape);
    assert(spline != nullptr);
    assert(spline->knots[0].position.x == 20.0f);
    assert(spline->knots[0].position.y == 20.0f);
    assert(spline->knots[0].out_handle.position.x == 28.0f);
    assert(spline->knots[0].out_handle.position.y == 22.0f);
    assert(spline->knots[0].in_handle.position.x == 12.0f);
    assert(spline->knots[0].in_handle.position.y == 18.0f);
}

void test_annotation_controller_direct_tool_moves_selected_point_handle() {
    AnnotationController controller;
    AnnotationDocument document;
    AnnotationSession session;
    AnnotationCategories categories;
    ensure_annotation_category(categories, "point");
    AnnotationFrame frame = make_controller_frame();

    AnnotationObject object;
    object.object_id = "point-1";
    object.category_index = 0U;
    object.shape = AnnotationPointShape{AnnotationPoint{12.0f, 16.0f}};
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));

    session.select_object(0U);
    assert(controller.set_active_tool(AnnotationToolKind::Direct, document, session));
    assert(controller.handle_handle_drag(document, session, frame, categories,
                                         AnnotationHandleId{0U, 0U, AnnotationHandleRole::Point}, 40, 44));

    const AnnotationObject* moved = document.object(0U);
    assert(moved != nullptr);
    const auto* point_shape = std::get_if<AnnotationPointShape>(&moved->shape);
    assert(point_shape != nullptr);
    assert(point_shape->point.x == 40.0f);
    assert(point_shape->point.y == 44.0f);
}

void test_annotation_document_mask_commands_update_generation() {
    AnnotationDocument document;
    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));
    assert(document.generation() == 1U);

    assert(document.apply(AnnotationPaintMaskCommand{
        0U,
        8,
        10,
        2,
        true,
        64U,
        64U,
    }));
    assert(document.generation() == 2U);
    const AnnotationObject* painted = document.object(0U);
    assert(painted != nullptr);
    const AnnotationMaskShape* painted_mask = annotation_object_mask_shape(*painted);
    assert(painted_mask != nullptr);
    assert(painted_mask->box.x2 > painted_mask->box.x1);
    assert(painted_mask->box.y2 > painted_mask->box.y1);
    assert(!painted_mask->mask.empty());

    assert(document.apply(AnnotationCleanupMaskCommand{
        0U,
        AnnotationMaskCleanupOp::FillHoles,
        1,
        64U,
        64U,
    }));
    assert(document.generation() == 3U);
}

void test_annotation_document_set_object_box_command_resets_dense_mask_state() {
    AnnotationDocument document;
    AnnotationObject object = make_test_annotation_object();
    assert(document.apply(AnnotationInsertObjectCommand{object, std::nullopt}));

    assert(document.apply(AnnotationPaintMaskCommand{
        0U,
        8,
        10,
        2,
        true,
        64U,
        64U,
    }));

    assert(document.apply(AnnotationSetObjectBoxCommand{
        0U,
        AnnotationBox{20, 22, 30, 34},
        64U,
        64U,
    }));
    const AnnotationObject* reset = document.object(0U);
    assert(reset != nullptr);
    const AnnotationBoxShape* box_shape = std::get_if<AnnotationBoxShape>(&reset->shape);
    assert(box_shape != nullptr);
    assert(box_shape->box.x1 == 20);
    assert(box_shape->box.y1 == 22);
    assert(box_shape->box.x2 == 30);
    assert(box_shape->box.y2 == 34);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_document_generation_tracks_commands);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_document_metadata_commands_update_generation);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_document_transform_commands_update_generation);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_document_mask_commands_update_generation);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_document_set_object_box_command_resets_dense_mask_state);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_tool_switch_clears_transient_session_state);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_session_overlay_revision_tracks_selection_only);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_tool_manager_resets_outgoing_tool_on_switch);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_renderer_builds_interaction_overlay_snapshot);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_workspace_model_projects_selection_and_crop);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_renderer_manual_overlay_snapshot_tracks_document_and_session_revisions);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_renderer_manual_overlay_snapshot_includes_vector_primitives);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_renderer_builds_editable_handles_for_selected_spline);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_renderer_exposes_latent_handles_for_selected_spline);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_remap_skeleton_annotation_object_to_category_preserves_named_nodes);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_create_object_seeds_spline_grouped_edit_state);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_create_object_commits_empty_skeleton_grouped_edit);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_create_object_keeps_skeleton_grouped_edit_open_for_topology);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_projected_scene_cache_refreshes_selection_without_invalidating_layout);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_editor_undo_redo_reselects_object_by_id_after_reorder);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_cancel_grouped_edit_for_selection_restores_prior_selection_and_cancels_changes);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_sidebar_semantic_edits_commit_as_single_undo_step);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_sidebar_mutation_effects_gate_assist_and_apply_follow_up_actions);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_mask_sidebar_edits_commit_cleanup_and_color_ranges_in_one_undo_step);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_spline_sidebar_edits_insert_knot_and_update_handle_mode);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_skeleton_sidebar_edits_update_active_joint_and_hide_selected_joint);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_renderer_hit_tests_non_box_geometry);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_canvas_click_creates_and_updates_point_objects);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_box_commit_updates_box_and_mask_tools);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_controller_direct_tool_moves_selected_box);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]", test_annotation_document_handle_commands_update_generation);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_document]",
                         test_annotation_controller_direct_tool_moves_selected_point_handle);
