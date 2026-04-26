#include "annotation/controller.h"
#include "annotation/document/document.h"
#include "annotation/document/edit.h"
#include "annotation/render/renderer.h"
#include "annotation/session.h"
#include "annotation/workflow.h"
#include "annotation/workspace/canvas.h"
#include "annotation_core.h"
#include "app.h"
#include "app_api.h"
#include "browser_host_adapters.h"
#include "browser_retained_frame_registry.h"
#include "browser_workspace_surface_bridge.h"
#include "canvas_layers.h"
#include "console_output.h"
#include "default_state.h"
#include "file_picker.h"
#include "gui/annotation/editor.h"
#include "gui/annotation/sidebar_actions.h"
#include "gui/annotation/workflow.h"
#include "gui/annotation/workspace/model.h"
#include "gui_settings.h"
#include "live/live_helpers.h"
#include "live_predict_controller.h"
#include "live_session_utils.h"
#include "local_train_controller.h"
#include "mmltk/rfdetr/draw_cuda.h"
#include "mmltk/runtime/async_runtime.h"
#include "mmltk/runtime/model_registry.h"
#include "mmltk_logging.h"
#include "model_input_ui.h"
#include "preview_rect_drag.h"
#include "remote_train_controller.h"
#include "rfdetr_module.h"
#include "rfdetr_workflows.h"
#include "source_runtime.h"
#include "train_command.h"
#include "vast_query_controller.h"
#include "view_state.h"

#include "mmltk/rfdetr/live_predict.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/model_config.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/validate.h"
#include "rfdetr/backends.h"

#if MMLTK_RFDETR_LIVE_CAPTURE
#include "mmltk/live/live_session_controller.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mmltk::gui {

void AppDeleter::operator()(App* app) const noexcept {
    delete app;
}

AppHandle make_app(std::string vast_api_key, std::string settings_path) {
    return AppHandle(new App(std::move(vast_api_key), std::move(settings_path)));
}

void drain_background_work(App& app) {
    app.drain_background_work();
}

void shutdown(App& app) {
    app.shutdown();
}

mmltk::browser::StateSnapshot browser_state_snapshot(App& app) {
    return app.browser_state_snapshot();
}

void apply_browser_intent(App& app, const mmltk::browser::IntentMessage& intent) {
    app.apply_browser_intent(intent);
}

namespace {

using namespace mmltk::rfdetr;
namespace console_output = mmltk::gui::console_output;

constexpr int kWhiteCropStrokeWidth = 2;
constexpr std::size_t kMaxUiCallbacksPerPoll = 32U;
constexpr auto kSettingsPersistencePollInterval = std::chrono::milliseconds(250);

template <typename T>
[[nodiscard]] const T* optional_value_ptr(const std::optional<T>& value) noexcept {
    if (!value.has_value()) {
        return nullptr;
    }
    return std::addressof(*value);
}

[[nodiscard]] nlohmann::json json_optional_index(const std::optional<std::size_t> index) {
    return index.has_value() ? nlohmann::json(*index) : nlohmann::json(nullptr);
}

[[nodiscard]] nlohmann::json json_optional_string(const std::optional<std::string>& value) {
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

[[nodiscard]] nlohmann::json annotation_box_to_json(const AnnotationBox& box) {
    return {
        {"x1", box.x1},
        {"y1", box.y1},
        {"x2", box.x2},
        {"y2", box.y2},
    };
}

[[nodiscard]] nlohmann::json annotation_point_to_json(const AnnotationPoint& point) {
    return {
        {"x", point.x},
        {"y", point.y},
    };
}

[[nodiscard]] nlohmann::json annotation_hsv_to_json(const AnnotationHsv& hsv) {
    return {
        {"hue_degrees", hsv.hue_degrees},
        {"saturation", hsv.saturation},
        {"value", hsv.value},
    };
}

[[nodiscard]] nlohmann::json annotation_color_tolerance_to_json(const AnnotationColorTolerance& tolerance) {
    return {
        {"hue_minus_pct", tolerance.hue_minus_pct},
        {"hue_plus_pct", tolerance.hue_plus_pct},
        {"saturation_minus_pct", tolerance.saturation_minus_pct},
        {"saturation_plus_pct", tolerance.saturation_plus_pct},
        {"value_minus_pct", tolerance.value_minus_pct},
        {"value_plus_pct", tolerance.value_plus_pct},
    };
}

[[nodiscard]] nlohmann::json annotation_color_range_to_json(const AnnotationColorRange& range) {
    return {
        {"center", annotation_hsv_to_json(range.center)},
        {"tolerance", annotation_color_tolerance_to_json(range.tolerance)},
        {"sampling", range.sampling},
    };
}

[[nodiscard]] float json_number_or(const nlohmann::json& object, const char* key, const float fallback) {
    if (const auto it = object.find(key); it != object.end() && it->is_number()) {
        return it->get<float>();
    }
    return fallback;
}

[[nodiscard]] AnnotationColorRange annotation_color_range_from_json(const nlohmann::json& json,
                                                                    const AnnotationColorRange& fallback) {
    if (!json.is_object()) {
        throw std::runtime_error("browser annotate mask color range payload must be a JSON object");
    }

    AnnotationColorRange range = fallback;
    if (const auto center_it = json.find("center"); center_it != json.end() && center_it->is_object()) {
        range.center.hue_degrees = json_number_or(*center_it, "hue_degrees", fallback.center.hue_degrees);
        range.center.saturation = json_number_or(*center_it, "saturation", fallback.center.saturation);
        range.center.value = json_number_or(*center_it, "value", fallback.center.value);
    }
    if (const auto tolerance_it = json.find("tolerance"); tolerance_it != json.end() && tolerance_it->is_object()) {
        range.tolerance.hue_minus_pct =
            json_number_or(*tolerance_it, "hue_minus_pct", fallback.tolerance.hue_minus_pct);
        range.tolerance.hue_plus_pct = json_number_or(*tolerance_it, "hue_plus_pct", fallback.tolerance.hue_plus_pct);
        range.tolerance.saturation_minus_pct =
            json_number_or(*tolerance_it, "saturation_minus_pct", fallback.tolerance.saturation_minus_pct);
        range.tolerance.saturation_plus_pct =
            json_number_or(*tolerance_it, "saturation_plus_pct", fallback.tolerance.saturation_plus_pct);
        range.tolerance.value_minus_pct =
            json_number_or(*tolerance_it, "value_minus_pct", fallback.tolerance.value_minus_pct);
        range.tolerance.value_plus_pct =
            json_number_or(*tolerance_it, "value_plus_pct", fallback.tolerance.value_plus_pct);
    }
    if (const auto sampling_it = json.find("sampling"); sampling_it != json.end() && sampling_it->is_boolean()) {
        range.sampling = sampling_it->get<bool>();
    }

    range.center.hue_degrees = std::clamp(range.center.hue_degrees, 0.0f, 360.0f);
    range.center.saturation = std::clamp(range.center.saturation, 0.0f, 1.0f);
    range.center.value = std::clamp(range.center.value, 0.0f, 1.0f);
    range.tolerance.hue_minus_pct = std::clamp(range.tolerance.hue_minus_pct, 0.0f, 100.0f);
    range.tolerance.hue_plus_pct = std::clamp(range.tolerance.hue_plus_pct, 0.0f, 100.0f);
    range.tolerance.saturation_minus_pct = std::clamp(range.tolerance.saturation_minus_pct, 0.0f, 100.0f);
    range.tolerance.saturation_plus_pct = std::clamp(range.tolerance.saturation_plus_pct, 0.0f, 100.0f);
    range.tolerance.value_minus_pct = std::clamp(range.tolerance.value_minus_pct, 0.0f, 100.0f);
    range.tolerance.value_plus_pct = std::clamp(range.tolerance.value_plus_pct, 0.0f, 100.0f);
    return range;
}

[[nodiscard]] nlohmann::json annotation_sidebar_spline_to_json(const AnnotationSidebarSelectedSplineViewModel& spline) {
    return {
        {"knot_count", spline.knot_count},
        {"segment_count", spline.segment_count},
        {"closed", spline.closed},
        {"active_knot_index", json_optional_index(spline.active_knot_index)},
        {"active_segment_index", json_optional_index(spline.active_segment_index)},
        {"active_handle_mode", spline.active_handle_mode.has_value()
                                   ? nlohmann::json(annotation_spline_handle_mode_name(*spline.active_handle_mode))
                                   : nlohmann::json(nullptr)},
        {"close_intent", spline.close_intent},
        {"reopen_requested", spline.reopen_requested},
        {"can_close", spline.can_close},
        {"can_reopen", spline.can_reopen},
        {"can_insert_active_segment_knot", spline.can_insert_active_segment_knot},
        {"can_delete_active_knot", spline.can_delete_active_knot},
        {"can_edit_active_handle_mode", spline.can_edit_active_handle_mode},
    };
}

[[nodiscard]] nlohmann::json annotation_sidebar_skeleton_to_json(
    const AnnotationSidebarSelectedSkeletonViewModel& skeleton) {
    nlohmann::json joints = nlohmann::json::array();
    for (const AnnotationSidebarSkeletonJointViewModel& joint : skeleton.joints) {
        joints.push_back({
            {"index", joint.index},
            {"label", joint.label},
            {"visible", joint.visible},
            {"active", joint.active},
        });
    }

    return {
        {"visible_joint_count", skeleton.visible_joint_count},
        {"total_joint_count", skeleton.total_joint_count},
        {"active_joint_index", json_optional_index(skeleton.active_joint_index)},
        {"active_joint_key", json_optional_string(skeleton.active_joint_key)},
        {"next_joint_index", json_optional_index(skeleton.next_joint_index)},
        {"next_joint_key", json_optional_string(skeleton.next_joint_key)},
        {"active_joint_visible", skeleton.active_joint_visible},
        {"reseed_requested", skeleton.reseed_requested},
        {"can_skip_joint", skeleton.can_skip_joint},
        {"can_hide_active_joint", skeleton.can_hide_active_joint},
        {"can_reactivate_active_joint", skeleton.can_reactivate_active_joint},
        {"can_reseed_active_joint", skeleton.can_reseed_active_joint},
        {"joints", std::move(joints)},
    };
}

[[nodiscard]] nlohmann::json annotation_sidebar_state_to_json(const AnnotationSidebarViewModel& model,
                                                              const bool has_annotation_frame, const int cleanup_radius,
                                                              const bool assist_available, const bool assist_running,
                                                              const std::string& assist_summary,
                                                              const std::string& assist_error) {
    nlohmann::json categories = nlohmann::json::array();
    for (const AnnotationSidebarClassViewModel& category : model.classes) {
        categories.push_back({
            {"id", category.id},
            {"name", category.name},
        });
    }

    nlohmann::json objects = nlohmann::json::array();
    for (const AnnotationSidebarObjectViewModel& object : model.objects) {
        objects.push_back({
            {"index", object.index},
            {"label", object.label},
            {"selected", object.selected},
        });
    }

    nlohmann::json payload{
        {"has_annotation_frame", has_annotation_frame},
        {"cleanup_radius", std::clamp(cleanup_radius, 1, 32)},
        {"assist_available", assist_available},
        {"assist_running", assist_running},
        {"assist_summary", assist_summary},
        {"assist_error", assist_error},
        {"preferred_new_object_category_index", model.preferred_new_object_category_index},
        {"has_classes", model.has_classes},
        {"can_undo", model.can_undo},
        {"can_redo", model.can_redo},
        {"can_create_objects", model.can_create_objects},
        {"can_delete_selected", model.can_delete_selected},
        {"can_redraw_selected_box", model.can_redraw_selected_box},
        {"can_edit_selected_mask", model.can_edit_selected_mask},
        {"has_selected_object", model.has_selected_object},
        {"selected_object_index", model.selected_object_index.has_value() ? nlohmann::json(*model.selected_object_index)
                                                                          : nlohmann::json(nullptr)},
        {"categories", std::move(categories)},
        {"objects", std::move(objects)},
    };

    if (model.selected_object.has_value()) {
        payload["selected_object"] = {
            {"enabled", model.selected_object->enabled},
            {"category_index", model.selected_object->category_index},
            {"shape_label", model.selected_object->shape_label},
            {"display_box", model.selected_object->display_box.has_value()
                                ? nlohmann::json(annotation_box_to_json(*model.selected_object->display_box))
                                : nlohmann::json(nullptr)},
            {"supports_mask_editing", model.selected_object->supports_mask_editing},
            {"sup", annotation_color_range_to_json(model.selected_object->sup)},
            {"nosup", annotation_color_range_to_json(model.selected_object->nosup)},
            {"point", model.selected_object->point.has_value()
                          ? nlohmann::json(annotation_point_to_json(*model.selected_object->point))
                          : nlohmann::json(nullptr)},
            {"spline", model.selected_object->spline.has_value()
                           ? nlohmann::json(annotation_sidebar_spline_to_json(*model.selected_object->spline))
                           : nlohmann::json(nullptr)},
            {"skeleton", model.selected_object->skeleton.has_value()
                             ? nlohmann::json(annotation_sidebar_skeleton_to_json(*model.selected_object->skeleton))
                             : nlohmann::json(nullptr)},
        };
    } else {
        payload["selected_object"] = nullptr;
    }

    return payload;
}

[[nodiscard]] const char* annotation_handle_role_name(const AnnotationHandleRole role) noexcept {
    switch (role) {
        case AnnotationHandleRole::Point:
            return "point";
        case AnnotationHandleRole::SplineKnot:
            return "spline_knot";
        case AnnotationHandleRole::SplineInHandle:
            return "spline_in_handle";
        case AnnotationHandleRole::SplineOutHandle:
            return "spline_out_handle";
        case AnnotationHandleRole::SkeletonNode:
            return "skeleton_node";
        case AnnotationHandleRole::None:
            break;
    }
    return "point";
}

[[nodiscard]] nlohmann::json annotation_workspace_geometry_to_json(const AnnotationVisibleGeometry& geometry) {
    nlohmann::json capture_points = nlohmann::json::array();
    for (const AnnotationPoint& point : geometry.capture_points) {
        capture_points.push_back(annotation_point_to_json(point));
    }

    nlohmann::json edges = nlohmann::json::array();
    for (const AnnotationSkeletonEdge& edge : geometry.edges) {
        edges.push_back({
            {"source_index", edge.source_index},
            {"target_index", edge.target_index},
        });
    }

    return {
        {"capture_points", std::move(capture_points)},
        {"edges", std::move(edges)},
        {"closed", geometry.closed},
    };
}

[[nodiscard]] nlohmann::json annotation_workspace_object_to_json(const AnnotationVisibleObject& object) {
    return {
        {"index", object.index},
        {"category_index", object.category_index},
        {"shape_type", annotation_shape_type_name(object.shape_type)},
        {"capture_box", annotation_box_to_json(object.capture_box)},
        {"fully_visible", object.fully_visible},
        {"geometry", annotation_workspace_geometry_to_json(object.geometry)},
    };
}

[[nodiscard]] nlohmann::json annotation_workspace_handle_to_json(const AnnotationFrame& frame,
                                                                 const AnnotationEditableHandle& handle) {
    std::optional<AnnotationPoint> tether_capture_point;
    if (handle.has_tether) {
        tether_capture_point =
            annotation_frame_point_to_capture_unclipped(handle.tether_frame_point, frame.view_x, frame.view_y);
    }

    return {
        {"object_index", handle.id.object_index},
        {"element_index", handle.id.element_index},
        {"role", annotation_handle_role_name(handle.id.role)},
        {"category_index", handle.category_index},
        {"capture_point", annotation_point_to_json(handle.capture_point)},
        {"tether_capture_point", tether_capture_point.has_value()
                                     ? nlohmann::json(annotation_point_to_json(*tether_capture_point))
                                     : nlohmann::json(nullptr)},
        {"materialized", handle.materialized},
    };
}

[[nodiscard]] nlohmann::json annotation_workspace_brush_preview_to_json(const AnnotationBrushPreview& brush_preview) {
    return {
        {"visible", brush_preview.visible},     {"capture_x", brush_preview.capture_x},
        {"capture_y", brush_preview.capture_y}, {"radius", std::max(1, brush_preview.radius)},
        {"erase", brush_preview.erase},
    };
}

[[nodiscard]] const AnnotationResolvedObject* find_resolved_annotation_object(
    const std::vector<AnnotationResolvedObject>& resolved_objects,
    const std::optional<std::size_t> object_index) noexcept {
    if (!object_index.has_value()) {
        return nullptr;
    }
    const auto it = std::find_if(
        resolved_objects.begin(), resolved_objects.end(),
        [object_index](const AnnotationResolvedObject& resolved) { return resolved.object_index == *object_index; });
    return it != resolved_objects.end() ? &(*it) : nullptr;
}

[[nodiscard]] std::string crop_annotation_mask_rle(const AnnotationResolvedObject& resolved,
                                                   const AnnotationBox& frame_box, const std::uint32_t frame_width,
                                                   const std::uint32_t frame_height) {
    if (!annotation_box_has_area(frame_box)) {
        return {};
    }
    const std::size_t expected_pixels = static_cast<std::size_t>(frame_width) * static_cast<std::size_t>(frame_height);
    if (resolved.mask.size() != expected_pixels) {
        return {};
    }

    const int box_width = frame_box.x2 - frame_box.x1;
    const int box_height = frame_box.y2 - frame_box.y1;
    if (box_width <= 0 || box_height <= 0) {
        return {};
    }

    std::vector<std::uint8_t> local_mask(static_cast<std::size_t>(box_width) * static_cast<std::size_t>(box_height),
                                         std::uint8_t{0});
    const auto row_width = static_cast<std::size_t>(box_width);
    for (int y = 0; y < box_height; ++y) {
        const std::size_t source_offset =
            (static_cast<std::size_t>(frame_box.y1 + y) * static_cast<std::size_t>(frame_width)) +
            static_cast<std::size_t>(frame_box.x1);
        const std::size_t target_offset = static_cast<std::size_t>(y) * row_width;
        std::copy_n(resolved.mask.begin() + static_cast<std::ptrdiff_t>(source_offset), row_width,
                    local_mask.begin() + static_cast<std::ptrdiff_t>(target_offset));
    }
    return encode_annotation_mask_rle(local_mask);
}

[[nodiscard]] std::optional<nlohmann::json> annotation_workspace_dense_mask_to_json(
    const AnnotationFrame& frame, const AnnotationResolvedObject& resolved) {
    const AnnotationBox frame_box = normalize_annotation_box(resolved.bbox, frame.width, frame.height);
    if (!annotation_box_has_area(frame_box)) {
        return std::nullopt;
    }

    const std::string mask_rle = crop_annotation_mask_rle(resolved, frame_box, frame.width, frame.height);
    if (mask_rle.empty()) {
        return std::nullopt;
    }

    return nlohmann::json{
        {"object_index", resolved.object_index},
        {"category_index", resolved.category_index},
        {"capture_box", annotation_box_to_json(annotation_box_from_frame(frame, frame_box))},
        {"mask_rle_encoding", "row_major_start_length"},
        {"mask_rle", mask_rle},
    };
}

[[nodiscard]] nlohmann::json annotation_workspace_visible_dense_masks_to_json(
    const AnnotationFrame& frame, const AnnotationProjectedScene& scene,
    const std::vector<AnnotationResolvedObject>& resolved_objects) {
    nlohmann::json dense_masks = nlohmann::json::array();
    for (const AnnotationVisibleObject& object : scene.visible_objects) {
        if (object.shape_type != AnnotationShapeType::Mask) {
            continue;
        }
        const AnnotationResolvedObject* resolved = find_resolved_annotation_object(resolved_objects, object.index);
        if (resolved == nullptr) {
            continue;
        }
        if (const std::optional<nlohmann::json> dense_mask = annotation_workspace_dense_mask_to_json(frame, *resolved);
            dense_mask.has_value()) {
            dense_masks.push_back(*dense_mask);
        }
    }
    return dense_masks;
}

[[nodiscard]] std::optional<nlohmann::json> annotation_workspace_selected_dense_mask_to_json(
    const AnnotationFrame& frame, const AnnotationProjectedScene& scene,
    const std::vector<AnnotationResolvedObject>& resolved_objects) {
    const AnnotationResolvedObject* resolved =
        find_resolved_annotation_object(resolved_objects, scene.selected_object_index);
    if (resolved == nullptr) {
        return std::nullopt;
    }
    return annotation_workspace_dense_mask_to_json(frame, *resolved);
}

[[nodiscard]] nlohmann::json annotation_workspace_scene_to_json(
    const AnnotationFrame& frame, const AnnotationProjectedScene& scene,
    const AnnotationBrushPreview* brush_preview = nullptr,
    const std::vector<AnnotationResolvedObject>* resolved_objects = nullptr) {
    nlohmann::json visible_objects = nlohmann::json::array();
    for (const AnnotationVisibleObject& object : scene.visible_objects) {
        visible_objects.push_back(annotation_workspace_object_to_json(object));
    }

    nlohmann::json editable_handles = nlohmann::json::array();
    for (const AnnotationEditableHandle& handle : scene.editable_handles) {
        editable_handles.push_back(annotation_workspace_handle_to_json(frame, handle));
    }

    nlohmann::json scene_json = {
        {"document_generation", scene.document_generation},
        {"selected_object_index", json_optional_index(scene.selected_object_index)},
        {"capture_width", annotation_frame_capture_width(frame)},
        {"capture_height", annotation_frame_capture_height(frame)},
        {"visible_objects", std::move(visible_objects)},
        {"editable_handles", std::move(editable_handles)},
    };
    if (brush_preview != nullptr) {
        scene_json["brush_preview"] = annotation_workspace_brush_preview_to_json(*brush_preview);
    }
    if (resolved_objects != nullptr) {
        const nlohmann::json visible_dense_masks =
            annotation_workspace_visible_dense_masks_to_json(frame, scene, *resolved_objects);
        if (!visible_dense_masks.empty()) {
            scene_json["visible_dense_masks"] = visible_dense_masks;
        }
        if (const std::optional<nlohmann::json> selected_dense_mask =
                annotation_workspace_selected_dense_mask_to_json(frame, scene, *resolved_objects);
            selected_dense_mask.has_value()) {
            scene_json["selected_dense_mask"] = *selected_dense_mask;
        }
    }
    return scene_json;
}

[[nodiscard]] std::uint64_t next_browser_frame_session_nonce() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] mmltk::live::LiveFrameId next_browser_frame_id(const std::uint64_t session_nonce,
                                                             std::uint64_t& sequence) noexcept {
    ++sequence;
    return mmltk::live::LiveFrameId{session_nonce, sequence};
}

[[nodiscard]] bool annotation_frame_uses_capture_subregion(const AnnotationFrame& frame) noexcept {
    return frame.view_x != 0U || frame.view_y != 0U ||
           (frame.capture_width > 0U && frame.capture_width != frame.width) ||
           (frame.capture_height > 0U && frame.capture_height != frame.height);
}

[[nodiscard]] mmltk::browser::CaptureRegion browser_capture_region_from_annotation_frame(
    const AnnotationFrame& frame) noexcept {
    return mmltk::browser::CaptureRegion{
        frame.view_x,
        frame.view_y,
        frame.width,
        frame.height,
    };
}

template <typename Bundle>
[[nodiscard]] RetainedBrowserImportedFrameSource make_retained_browser_imported_frame_source(
    const mmltk::browser::FrameSlotDescriptor& descriptor, const Bundle& bundle, const int cuda_device_index,
    const std::optional<LinuxImportedFrameLifecycleContract>& lifecycle_contract = std::nullopt) {
    RetainedBrowserImportedFrameSource source;
    source.descriptor = descriptor;
    mmltk::browser::normalize_frame_slot_native_import_metadata(source.descriptor);
    apply_linux_imported_frame_source_contract(
        source,
        LinuxImportedFrameSourceContract{
            LinuxImportedImageSource{
                bundle.data == 0U ? LinuxImportedImageSourceKind::Unknown
                                  : LinuxImportedImageSourceKind::CudaDevicePointer,
                static_cast<std::uintptr_t>(bundle.data),
                cuda_device_index,
            },
            LinuxImportedSyncSource{
                bundle.ready_event == nullptr ? LinuxImportedSyncHandleKind::None
                                              : LinuxImportedSyncHandleKind::CudaEvent,
                reinterpret_cast<std::uintptr_t>(bundle.ready_event),
                descriptor.ready_sync.value,
            },
            LinuxImportedSyncSource{
                bundle.stream == nullptr ? LinuxImportedSyncHandleKind::None : LinuxImportedSyncHandleKind::CudaStream,
                reinterpret_cast<std::uintptr_t>(bundle.stream),
                0U,
            },
        });
    if (lifecycle_contract.has_value()) {
        apply_linux_imported_frame_lifecycle_contract(source, *lifecycle_contract);
    }
    normalize_retained_browser_imported_frame_source(source);
    return source;
}

struct RecentJobTileConfig {
    std::string_view label;
    const char* tile_id;
    const char* heading;
    const char* output_id;
    const char* waiting_message;
    const char* error_banner_id = nullptr;
};

void log_gui_error(const char* context, const std::string& message) {
    if (message.empty()) {
        return;
    }
    mmltk::logging::logger("gui")->error("mmltk gui {}: {}", context, message);
}

std::string annotation_source_signature(const SourceSelectionState& source) {
    return std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", static_cast<int>(source.kind), source.compiled_path,
                       source.single_image_path, source.image_directory, source.recursive, source.device_index,
                       source.capture_width, source.capture_height, source.capture_fps, source.v4l2_buffer_count);
}

std::vector<RemoteGpuFamily> selected_remote_gpu_families(const std::array<bool, 5>& enabled_families) {
    std::vector<RemoteGpuFamily> families;
    for (int index = 0; index < static_cast<int>(enabled_families.size()); ++index) {
        if (!enabled_families[static_cast<size_t>(index)]) {
            continue;
        }
        families.push_back(static_cast<RemoteGpuFamily>(index));
    }
    return families;
}

const ModelPresetConfig& resolve_selected_preset(std::string& preset_name) {
    if (const auto* preset = find_model_preset(preset_name)) {
        return *preset;
    }
    const auto* fallback = find_model_preset(kDefaultGuiPresetName);
    if (fallback == nullptr) {
        throw std::runtime_error("missing default RF-DETR preset");
    }
    preset_name = std::string(fallback->preset_name);
    return *fallback;
}

TrainRecipeConfig current_train_recipe(const std::string& preset_name, TrainOptimizerMode optimizer) {
    return resolve_train_recipe(preset_name, train_optimizer_kind(optimizer));
}

[[nodiscard]] mmltk::browser::JobState browser_job_state_from_local_train(const LocalTrainSessionState& state) {
    mmltk::browser::JobState job;
    job.running = state.running;
    job.label = state.label.empty() ? "train.local" : state.label;
    job.summary = state.last_summary;
    job.error = state.last_error;
    job.output_tail = state.output_tail;
    return job;
}

[[nodiscard]] mmltk::browser::JobState browser_job_state_from_remote_train(const RemoteTrainSessionState& state) {
    mmltk::browser::JobState job;
    job.running = state.request_running || state.refresh_running ||
                  (state.instance.has_value() && remote_train_instance_running(*state.instance));
    job.label = "train.remote";
    job.summary = state.request_running && !state.action_label.empty() ? state.action_label : state.last_summary;
    job.error = state.last_error;
    job.output_tail = state.output_tail;
    return job;
}

[[nodiscard]] nlohmann::json local_train_progress_to_json(const TrainArtifactProgress& progress) {
    return nlohmann::json{
        {"phase", progress.phase},
        {"epoch", progress.epoch},
        {"total_epochs", progress.total_epochs},
        {"completed_batches", progress.completed_batches},
        {"total_batches", progress.total_batches},
        {"completed_waves", progress.completed_waves},
        {"optimizer_steps", progress.optimizer_steps},
        {"steps_per_epoch", progress.steps_per_epoch},
        {"train_lanes", progress.train_lanes},
        {"train_loss", progress.train_loss},
        {"class_loss", progress.class_loss},
        {"box_loss", progress.box_loss},
        {"step_loss", progress.step_loss},
        {"step_class_loss", progress.step_class_loss},
        {"step_box_loss", progress.step_box_loss},
        {"batches_per_second", progress.batches_per_second},
        {"images_per_second", progress.images_per_second},
        {"elapsed_seconds", progress.elapsed_seconds},
        {"val_loss", progress.val_loss},
        {"val_bbox_ap", progress.val_bbox_ap},
        {"val_mask_ap", progress.val_mask_ap},
        {"checkpoint_path", progress.checkpoint_path},
    };
}

[[nodiscard]] nlohmann::json local_train_runtime_to_json(const LocalTrainSessionState& state) {
    nlohmann::json runtime = {
        {"running", state.running},
        {"stop_requested", state.stop_requested},
        {"force_kill_requested", state.force_kill_requested},
        {"label", state.label.empty() ? "train.local" : state.label},
        {"summary", state.last_summary},
        {"error", state.last_error},
        {"pid", state.pid >= 0 ? nlohmann::json(state.pid) : nlohmann::json(nullptr)},
        {"process_group_id",
         state.process_group_id >= 0 ? nlohmann::json(state.process_group_id) : nlohmann::json(nullptr)},
        {"exit_code", state.exit_code >= 0 ? nlohmann::json(state.exit_code) : nlohmann::json(nullptr)},
        {"output_dir", state.output_dir.empty() ? std::string{} : state.output_dir.string()},
        {"output_tail", state.output_tail},
        {"progress",
         state.progress.has_value() ? local_train_progress_to_json(*state.progress) : nlohmann::json(nullptr)},
    };
    return runtime;
}

[[nodiscard]] nlohmann::json remote_train_session_to_json(const RemoteTrainSessionState& state) {
    nlohmann::json session = {
        {"request_running", state.request_running},
        {"refresh_running", state.refresh_running},
        {"action_label", state.action_label},
        {"last_summary", state.last_summary},
        {"last_error", state.last_error},
        {"output_tail", state.output_tail},
        {"instance_present", state.instance.has_value()},
        {"instance_running", state.instance.has_value() ? nlohmann::json(remote_train_instance_running(*state.instance))
                                                        : nlohmann::json(false)},
        {"offer_id", state.instance.has_value() ? nlohmann::json(state.instance->offer_id) : nlohmann::json(nullptr)},
        {"instance_id",
         state.instance.has_value() ? nlohmann::json(state.instance->instance_id) : nlohmann::json(nullptr)},
        {"actual_status", std::string{}},
        {"current_state", std::string{}},
        {"next_state", std::string{}},
        {"intended_status", std::string{}},
        {"image_uuid", std::string{}},
        {"label", std::string{}},
        {"num_gpus", 0},
        {"gpu_name", std::string{}},
        {"ssh_host", std::string{}},
        {"ssh_port", nlohmann::json(nullptr)},
        {"public_ipaddr", std::string{}},
        {"status_message", std::string{}},
        {"jupyter_token", std::string{}},
        {"ports", std::string{}},
        {"duration_seconds", 0.0},
    };
    if (!state.instance.has_value()) {
        return session;
    }

    const RemoteTrainInstanceState& instance = *state.instance;
    session["actual_status"] = instance.actual_status;
    session["current_state"] = instance.current_state;
    session["next_state"] = instance.next_state;
    session["intended_status"] = instance.intended_status;
    session["image_uuid"] = instance.image_uuid;
    session["label"] = instance.label;
    session["num_gpus"] = instance.num_gpus;
    session["gpu_name"] = instance.gpu_name;
    session["ssh_host"] = instance.ssh_host;
    session["ssh_port"] = instance.ssh_port > 0 ? nlohmann::json(instance.ssh_port) : nlohmann::json(nullptr);
    session["public_ipaddr"] = instance.public_ipaddr;
    session["status_message"] = instance.status_message;
    session["jupyter_token"] = instance.jupyter_token;
    session["ports"] = instance.ports;
    session["duration_seconds"] = instance.duration_seconds;
    return session;
}

[[nodiscard]] nlohmann::json remote_offer_to_json(const VastOfferSummary& offer) {
    return nlohmann::json{
        {"offer_id", offer.offer_id},       {"family", remote_gpu_family_label(offer.family)},
        {"gpu_name", offer.gpu_name},       {"num_gpus", offer.num_gpus},
        {"gpu_ram", offer.gpu_ram},         {"dph", offer.dph},
        {"dlperf", offer.dlperf},           {"dlperf_usd", offer.dlperf_usd},
        {"reliability", offer.reliability}, {"inet_down", offer.inet_down},
        {"disk_space", offer.disk_space},   {"geolocation", offer.geolocation},
    };
}

[[nodiscard]] nlohmann::json train_remote_query_to_json(const VastQueryController& controller,
                                                        const bool api_key_configured) {
    const VastQueryState& state = controller.state();
    nlohmann::json results = nlohmann::json::array();
    for (const VastOfferSummary& offer : state.results) {
        results.push_back(remote_offer_to_json(offer));
    }

    const std::optional<VastOfferSummary>& armed_offer = controller.armed_offer();
    return nlohmann::json{
        {"api_key_configured", api_key_configured},
        {"running", state.running},
        {"last_summary", state.last_summary},
        {"last_error", state.last_error},
        {"armed_offer_id", armed_offer.has_value() ? nlohmann::json(armed_offer->offer_id) : nlohmann::json(nullptr)},
        {"armed_offer_summary", controller.armed_offer_summary()},
        {"results", std::move(results)},
    };
}

[[nodiscard]] bool should_surface_local_train_job(const LocalTrainSessionState& state, const bool job_running,
                                                  const View current_view) {
    return !job_running && (current_view == View::Train || state.running || !state.last_summary.empty() ||
                            !state.last_error.empty() || !state.output_tail.empty());
}

[[nodiscard]] bool should_surface_remote_train_job(const RemoteTrainSessionState& state, const bool job_running,
                                                   const View current_view) {
    return !job_running && (current_view == View::Train || state.request_running || state.refresh_running ||
                            state.instance.has_value() || !state.last_summary.empty() || !state.last_error.empty() ||
                            !state.output_tail.empty());
}

struct LiveImportedBrowserPublicationSlot {
    mmltk::live::BgrPitchedDeviceBuffer upload_buffer;
    mmltk::live::PinnedUploadBuffer<std::uint8_t> host_upload_buffer;
    mmltk::live::RgbaPitchedDeviceBuffer device_buffer;
    mmltk::live::CudaStreamHandle stream;
    mmltk::live::CudaEventHandle ready_event;
    int cuda_device_index = -1;
    std::optional<LinuxImportedFrameLifecycleContract> lifecycle_contract;
    RetainedBrowserFrameRegistry::ReleaseFn release_publication_resources;
    std::uint32_t retain_count = 0;

    [[nodiscard]] bool retained() const noexcept {
        return retain_count > 0;
    }

    void reset_publication_resources() {
        lifecycle_contract.reset();
        if (!release_publication_resources) {
            return;
        }

        RetainedBrowserFrameRegistry::ReleaseFn release_fn = std::move(release_publication_resources);
        release_publication_resources = {};
        release_fn();
    }

    void begin_retained_publication(std::optional<LinuxImportedFrameLifecycleContract> lifecycle = std::nullopt,
                                    RetainedBrowserFrameRegistry::ReleaseFn release_fn = {}) {
        reset_publication_resources();
        lifecycle_contract = lifecycle;
        release_publication_resources = std::move(release_fn);
        retain_count = 1;
    }

    void release_retained_publication() {
        if (retain_count == 0) {
            return;
        }
        --retain_count;
        if (retain_count > 0) {
            return;
        }
        reset_publication_resources();
    }

    void abandon_retained_publication() {
        retain_count = 0;
        reset_publication_resources();
    }
};

LiveImportedBrowserPublicationSlot& acquire_native_publication_slot(
    std::vector<std::unique_ptr<LiveImportedBrowserPublicationSlot>>& slots, const int cuda_device_index,
    const std::uint32_t width, const std::uint32_t height, const char* buffer_context, const char* stream_context,
    const char* event_context) {
    for (auto& slot : slots) {
        if (slot != nullptr && !slot->retained()) {
            if (slot->cuda_device_index != cuda_device_index) {
                const int previous_device = slot->cuda_device_index;
                if (previous_device >= 0) {
                    mmltk::live::ensure_cuda_ok(cudaSetDevice(previous_device),
                                                "cudaSetDevice for native publication slot reset");
                }
                slot->upload_buffer.reset();
                slot->host_upload_buffer.reset();
                slot->device_buffer.reset();
                slot->stream.reset();
                slot->ready_event.reset();
                mmltk::live::ensure_cuda_ok(cudaSetDevice(cuda_device_index),
                                            "cudaSetDevice for native publication slot reuse");
                slot->cuda_device_index = cuda_device_index;
            }
            slot->device_buffer.ensure_dimensions(width, height, buffer_context);
            if (slot->stream.empty()) {
                slot->stream.create_with_highest_priority(stream_context);
            }
            if (slot->ready_event.empty()) {
                slot->ready_event.create(cudaEventDisableTiming, event_context);
            }
            slot->cuda_device_index = cuda_device_index;
            return *slot;
        }
    }

    auto slot = std::make_unique<LiveImportedBrowserPublicationSlot>();
    slot->device_buffer.ensure_dimensions(width, height, buffer_context);
    slot->stream.create_with_highest_priority(stream_context);
    slot->ready_event.create(cudaEventDisableTiming, event_context);
    slot->cuda_device_index = cuda_device_index;
    LiveImportedBrowserPublicationSlot* slot_ptr = slot.get();
    slots.push_back(std::move(slot));
    return *slot_ptr;
}

}  // namespace

struct App::JobOutcome {
    std::string summary;
    std::optional<StillImagePreview> preview;
};

struct App::JobState {
    bool running = false;
    std::string label;
    std::string last_summary;
    std::string last_error;
    std::string output_tail;
    std::mutex output_mutex;
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct App::Impl {
    Impl(std::string vast_api_key, std::string settings_path);
    ~Impl();

    void initialize(App& app);

    View current_view_ = View::Train;
    std::string selected_preset_name_;
    UiSettingsState ui_settings_{};
    TrainViewState train_;
    ValidateViewState validate_;
    PredictViewState predict_;
    AnnotateViewState annotate_;
    ExportViewState export_;
    JobState job_;
    std::vector<mmltk::rfdetr::PredictImageInput> annotate_inputs_;
    std::string annotate_source_signature_;
    std::size_t annotate_current_input_index_ = 0;
    std::optional<AnnotationFrame> annotate_frame_;
    AnnotationDocument annotate_document_{};
    std::vector<AnnotationResolvedObject> annotate_resolved_instances_;
    AnnotationSession annotate_session_{};
    AnnotationController annotate_controller_{};
    AnnotationCategories annotate_categories_;
    bool annotate_categories_loaded_ = false;
    std::string annotate_categories_output_dir_;
    std::string annotate_pending_class_name_;
    int annotate_brush_radius_ = 12;
    int annotate_cleanup_radius_ = 1;
    AnnotationWorkflowRuntime annotate_workflow_{};
    float annotate_canvas_scale_ = 0.0f;
    float annotate_canvas_pan_x_ = 0.0f;
    float annotate_canvas_pan_y_ = 0.0f;
    bool annotate_canvas_auto_fit_ = true;
    bool annotate_canvas_panning_ = false;
    bool annotate_prepare_running_ = false;
    bool annotate_frame_load_running_ = false;
    std::uint64_t annotate_prepare_request_id_ = 0;
    std::uint64_t annotate_frame_request_id_ = 0;
    bool annotate_assist_running_ = false;
    std::string annotate_assist_summary_;
    std::string annotate_assist_error_;
    bool annotate_save_running_ = false;
    std::string annotate_save_summary_;
    std::string annotate_save_error_;
    std::unique_ptr<mmltk::live::LiveSessionController> live_controller_;
    std::unique_ptr<mmltk::live::LiveSessionStatus> live_session_status_;
    ActiveLiveMode active_live_mode_ = ActiveLiveMode::None;
    mmltk::runtime::UiCallbackQueue ui_callbacks_;
    mmltk::runtime::BackgroundExecutor background_executor_{2};
    LivePredictController live_predict_controller_{};
    VastQueryController vast_query_controller_;
    RemoteTrainController remote_train_controller_;
    LocalTrainController local_train_controller_;
    std::string live_static_preview_source_name_;
    std::string live_preview_error_;
    bool live_preview_fit_to_capture_ = false;
    bool live_crop_overlay_mode_ = false;
    bool shutting_down_ = false;
    PreviewRectDragSession live_crop_drag_session_{};
    FileBrowseField active_file_browse_field_ = FileBrowseField::None;
    std::string active_browser_dialog_id_;
    std::string active_browser_dialog_title_;
    std::string picker_error_;
    std::string vast_api_key_;
    mmltk::runtime::ModelRegistry model_registry_;
    GuiSettingsPersistence settings_persistence_;
    std::chrono::steady_clock::time_point next_settings_persistence_poll_{};
    std::unique_ptr<AnnotationWorkflowCoordinator> annotation_workflow_coordinator_;
    std::unique_ptr<BrowserPublicationState> browser_publication_state_;
    std::optional<BrowserWorkspaceViewportState> browser_workspace_viewport_state_;
    std::atomic<std::shared_ptr<const std::function<void()>>> evented_work_wake_callback_;
};

struct App::BrowserPublicationState {
    struct PredictStaticPreviewSource {
        std::string source_name;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> pixels_bgr;

        [[nodiscard]] bool ready() const noexcept {
            return width > 0U && height > 0U && pixels_bgr != nullptr && !pixels_bgr->empty();
        }
    };

    BrowserWorkspaceSurfaceBridge workspace_surface_bridge;
    mmltk::live::LiveSessionController* live_controller = nullptr;
    std::optional<int> live_cuda_device_index;
    std::optional<PredictStaticPreviewSource> predict_preview_source;
    std::vector<std::unique_ptr<LiveImportedBrowserPublicationSlot>> retained_publication_slots;
    std::uint64_t predict_session_nonce = next_browser_frame_session_nonce();
    std::uint64_t annotate_session_nonce = next_browser_frame_session_nonce();
    std::uint64_t predict_sequence = 0;
    std::uint64_t annotate_workspace_sequence = 0;
};

namespace {

struct RetainedBrowserUploadFrame {
    CUdeviceptr data = 0;
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
};

void publish_workspace_surface(BrowserWorkspaceSurfaceBridge& bridge, const BrowserWorkspaceSurfaceOwner owner,
                               RetainedBrowserImportedFrameSource source,
                               BrowserWorkspaceSurfaceBridge::ReleaseFn release_fn,
                               const std::optional<mmltk::live::LiveFrameId> live_frame_id = std::nullopt) {
    normalize_retained_browser_imported_frame_source(source);
    bridge.publish(owner, std::move(source), std::move(release_fn), live_frame_id);
}

template <typename PublishedFrameState>
void publish_retained_imported_browser_frame(
    PublishedFrameState& published, std::vector<std::unique_ptr<LiveImportedBrowserPublicationSlot>>& publication_slots,
    const std::span<const std::uint8_t> pixels_bgr, const int cuda_device_index,
    const BrowserWorkspaceSurfaceOwner owner, std::string slot_name, const std::uint32_t slot_index,
    const mmltk::live::LiveFrameId& frame_id, const mmltk::browser::CaptureRegion capture_region,
    const std::uint32_t width, const std::uint32_t height, const std::uint64_t source_row_stride_bytes,
    const std::uint64_t ready_ns, const bool short_frame) {
    const auto required_bytes =
        static_cast<std::size_t>(mmltk::browser::frame_byte_length(source_row_stride_bytes, height));
    if (pixels_bgr.size() < required_bytes) {
        throw std::runtime_error(
            "retained browser imported frame upload source is smaller than the "
            "published byte length");
    }

    LiveImportedBrowserPublicationSlot& publication_slot = acquire_native_publication_slot(
        publication_slots, cuda_device_index, width, height, "cudaMallocPitch for retained browser frame publication",
        "cudaStreamCreateWithPriority for retained browser frame publication",
        "cudaEventCreateWithFlags for retained browser frame publication");
    publication_slot.upload_buffer.ensure_dimensions(width, height,
                                                     "cudaMallocPitch for retained browser frame upload");
    publication_slot.host_upload_buffer.ensure_capacity(required_bytes,
                                                        "cudaHostAlloc for retained browser frame upload");
    std::copy_n(pixels_bgr.data(), required_bytes, publication_slot.host_upload_buffer.data());

    mmltk::live::ensure_cuda_ok(
        cudaMemcpy2DAsync(mmltk::live::device_ptr_as_void(publication_slot.upload_buffer.data()),
                          publication_slot.upload_buffer.pitch_bytes(), publication_slot.host_upload_buffer.data(),
                          source_row_stride_bytes, static_cast<std::size_t>(width) * 3U, height, cudaMemcpyHostToDevice,
                          publication_slot.stream.get()),
        "cudaMemcpy2DAsync for retained browser frame upload");
    mmltk::rfdetr::launch_copy_bgr_to_rgba_pitched(
        mmltk::live::device_ptr_as_bytes(publication_slot.upload_buffer.data()),
        publication_slot.upload_buffer.pitch_bytes(),
        mmltk::live::device_ptr_as_bytes(publication_slot.device_buffer.data()),
        publication_slot.device_buffer.pitch_bytes(), static_cast<int>(width), static_cast<int>(height), 255U,
        publication_slot.stream.get());
    mmltk::live::ensure_cuda_ok(cudaPeekAtLastError(),
                                "launch_copy_bgr_to_rgba_pitched for retained browser frame publication");
    mmltk::live::ensure_cuda_ok(cudaEventRecord(publication_slot.ready_event.get(), publication_slot.stream.get()),
                                "cudaEventRecord for retained browser frame publication");

    const RetainedBrowserUploadFrame publication_frame{
        publication_slot.device_buffer.data(),
        publication_slot.ready_event.get(),
        publication_slot.stream.get(),
    };
    mmltk::browser::FrameSlotDescriptor descriptor = mmltk::browser::frame_slot_from_bgr_frame(
        std::move(slot_name), slot_index, frame_id, capture_region, width, height,
        static_cast<std::uint64_t>(publication_slot.device_buffer.pitch_bytes()), ready_ns, short_frame);
    publication_slot.begin_retained_publication();
    try {
        publish_workspace_surface(
            published.workspace_surface_bridge, owner,
            make_retained_browser_imported_frame_source(descriptor, publication_frame, cuda_device_index),
            [slot = &publication_slot]() { slot->release_retained_publication(); });
    } catch (...) {
        if (publication_slot.ready_event.get() != nullptr) {
            (void)cudaEventSynchronize(publication_slot.ready_event.get());
        }
        publication_slot.abandon_retained_publication();
        throw;
    }
}

#if MMLTK_RFDETR_LIVE_CAPTURE

void release_live_workspace_slot_silently(mmltk::live::LiveSessionController* controller,
                                          const std::uint32_t slot_index) noexcept {
    if (controller == nullptr) {
        return;
    }
    try {
        controller->release_workspace(slot_index);
    } catch (const std::exception& error) {
        mmltk::logging::logger("gui")->error("failed to release imported browser workspace slot {}: {}", slot_index,
                                             error.what());
    }
}

[[nodiscard]] bool live_frame_not_newer_than_current_publication(const BrowserWorkspaceSurfaceBridge& bridge,
                                                                 const mmltk::live::LiveFrameId& frame_id) noexcept {
    if (bridge.owner() != BrowserWorkspaceSurfaceOwner::Live) {
        return false;
    }
    const std::optional<mmltk::live::LiveFrameId> current = bridge.preview().live_frame_id;
    if (!current.has_value() || current->session_nonce != frame_id.session_nonce) {
        return false;
    }
    return frame_id.sequence <= current->sequence;
}

#endif

struct BrowserPreviewSnapshot {
    bool initialized = false;
    bool has_frame = false;
    mmltk::live::LiveCaptureRegion displayed_region{};
    std::uint64_t last_frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    std::string last_error;
    bool interop_failed = false;
};

}  // namespace

class AnnotationWorkflowCoordinator {
   public:
    friend class App;

    explicit AnnotationWorkflowCoordinator(App& app) noexcept : app_(app) {}

    void shutdown();
    void poll();
    void invalidate_preview();
    void cancel_canvas_interactions();
    void reset_instances();
    void sync_categories();
    void prepare_source();
    void load_current_frame();
    void step_current_frame(int step);
    void handle_setup_browse_request(AnnotationSetupBrowseRequest browse_request);
    void load_frame(AnnotationFrame frame);
    void submit_preview();
    bool request_save_now();
    void start_live_session();
    void stop_live_session();
    [[nodiscard]] bool live_running() const;

   private:
    bool dispatch_save_now(const AnnotationSaveTabPlan& save_tab_plan);
    [[nodiscard]] bool ensure_live_annotation_frame_pixels(std::string* error_message) const;
    [[nodiscard]] AnnotationWorkflowSaveRequest make_save_request() const;
    void reset_live_session_state(bool clear_live_preview_error, App::ActiveLiveMode active_mode);
    void log_annotation_resolve_error(const char* error_context, const std::string& error) const;
    [[nodiscard]] std::optional<AnnotationSaveSnapshot> make_current_save_snapshot(bool refresh_live_frame,
                                                                                   const char* resolve_error_context);
    [[nodiscard]] bool launch_save(AnnotationSaveSnapshot save_snapshot, bool live_mode);

    App& app_;
};

App::Impl::Impl(std::string vast_api_key, std::string settings_path)
    : vast_query_controller_(background_executor_, ui_callbacks_),
      remote_train_controller_(background_executor_, ui_callbacks_),
      local_train_controller_(background_executor_, ui_callbacks_),
      vast_api_key_(std::move(vast_api_key)),
      settings_persistence_(std::move(settings_path)) {}

App::Impl::~Impl() = default;

void App::Impl::initialize(App& app) {
    model_registry_.register_module(make_rfdetr_model_module());
    apply_default_gui_state(selected_preset_name_, train_, validate_, predict_, annotate_, export_);
    bool loaded_settings = false;
    {
        GuiSettingsSnapshot snap = app.make_gui_settings_snapshot();
        if (settings_persistence_.load(snap)) {
            loaded_settings = true;
            current_view_ = snap.current_view;
            selected_preset_name_ = snap.selected_preset;
        }
    }
    ui_settings_.content_scale = 1.0f;

    const std::string requested_preset = selected_preset_name_;
    resolve_selected_preset(selected_preset_name_);
    if (!loaded_settings || selected_preset_name_ != requested_preset) {
        app.apply_selected_preset_defaults();
    }
    local_train_controller_.initialize(train_.local_device_ids);

    (void)annotate_controller_.set_active_tool(AnnotationToolKind::Direct, annotate_document_, annotate_session_);
    annotation_workflow_coordinator_ = std::make_unique<AnnotationWorkflowCoordinator>(app);
    browser_publication_state_ = std::make_unique<BrowserPublicationState>();
}

#define current_view_ impl_->current_view_
#define selected_preset_name_ impl_->selected_preset_name_
#define ui_settings_ impl_->ui_settings_
#define train_ impl_->train_
#define validate_ impl_->validate_
#define predict_ impl_->predict_
#define annotate_ impl_->annotate_
#define export_ impl_->export_
#define job_ impl_->job_
#define annotate_inputs_ impl_->annotate_inputs_
#define annotate_source_signature_ impl_->annotate_source_signature_
#define annotate_current_input_index_ impl_->annotate_current_input_index_
#define annotate_frame_ impl_->annotate_frame_
#define annotate_document_ impl_->annotate_document_
#define annotate_resolved_instances_ impl_->annotate_resolved_instances_
#define annotate_session_ impl_->annotate_session_
#define annotate_controller_ impl_->annotate_controller_
#define annotate_categories_ impl_->annotate_categories_
#define annotate_categories_loaded_ impl_->annotate_categories_loaded_
#define annotate_categories_output_dir_ impl_->annotate_categories_output_dir_
#define annotate_pending_class_name_ impl_->annotate_pending_class_name_
#define annotate_brush_radius_ impl_->annotate_brush_radius_
#define annotate_cleanup_radius_ impl_->annotate_cleanup_radius_
#define annotate_workflow_ impl_->annotate_workflow_
#define annotate_canvas_scale_ impl_->annotate_canvas_scale_
#define annotate_canvas_pan_x_ impl_->annotate_canvas_pan_x_
#define annotate_canvas_pan_y_ impl_->annotate_canvas_pan_y_
#define annotate_canvas_auto_fit_ impl_->annotate_canvas_auto_fit_
#define annotate_canvas_panning_ impl_->annotate_canvas_panning_
#define annotate_prepare_running_ impl_->annotate_prepare_running_
#define annotate_frame_load_running_ impl_->annotate_frame_load_running_
#define annotate_prepare_request_id_ impl_->annotate_prepare_request_id_
#define annotate_frame_request_id_ impl_->annotate_frame_request_id_
#define annotate_assist_running_ impl_->annotate_assist_running_
#define annotate_assist_summary_ impl_->annotate_assist_summary_
#define annotate_assist_error_ impl_->annotate_assist_error_
#define annotate_save_running_ impl_->annotate_save_running_
#define annotate_save_summary_ impl_->annotate_save_summary_
#define annotate_save_error_ impl_->annotate_save_error_
#define live_controller_ impl_->live_controller_
#define live_session_status_ impl_->live_session_status_
#define active_live_mode_ impl_->active_live_mode_
#define ui_callbacks_ impl_->ui_callbacks_
#define background_executor_ impl_->background_executor_
#define live_predict_controller_ impl_->live_predict_controller_
#define vast_query_controller_ impl_->vast_query_controller_
#define remote_train_controller_ impl_->remote_train_controller_
#define local_train_controller_ impl_->local_train_controller_
#define live_static_preview_source_name_ impl_->live_static_preview_source_name_
#define live_preview_error_ impl_->live_preview_error_
#define live_preview_fit_to_capture_ impl_->live_preview_fit_to_capture_
#define live_crop_overlay_mode_ impl_->live_crop_overlay_mode_
#define shutting_down_ impl_->shutting_down_
#define live_crop_drag_session_ impl_->live_crop_drag_session_
#define active_file_browse_field_ impl_->active_file_browse_field_
#define active_browser_dialog_id_ impl_->active_browser_dialog_id_
#define active_browser_dialog_title_ impl_->active_browser_dialog_title_
#define picker_error_ impl_->picker_error_
#define vast_api_key_ impl_->vast_api_key_
#define model_registry_ impl_->model_registry_
#define settings_persistence_ impl_->settings_persistence_
#define annotation_workflow_coordinator_ impl_->annotation_workflow_coordinator_
#define browser_publication_state_ impl_->browser_publication_state_
#define browser_workspace_viewport_state_ impl_->browser_workspace_viewport_state_

App::App(std::string vast_api_key, std::string settings_path)
    : impl_(std::make_unique<Impl>(std::move(vast_api_key), std::move(settings_path))) {
    impl_->initialize(*this);
}

App::~App() noexcept {
    try {
        shutdown();
    } catch (const std::exception& error) {
        mmltk::logging::logger("gui")->error("app shutdown failed during destruction: {}", error.what());
    } catch (...) {
        mmltk::logging::logger("gui")->error("app shutdown failed during destruction with an unknown exception");
    }
}

void App::set_evented_work_wake_callback(std::function<void()> callback) {
    if (!callback) {
        impl_->evented_work_wake_callback_.store(nullptr, std::memory_order_release);
        ui_callbacks_.set_wake_callback({});
        refresh_live_workspace_ready_callbacks();
        return;
    }

    auto shared_callback = std::make_shared<const std::function<void()>>(std::move(callback));
    impl_->evented_work_wake_callback_.store(shared_callback, std::memory_order_release);
    ui_callbacks_.set_wake_callback(*shared_callback);
    refresh_live_workspace_ready_callbacks();
}

void App::notify_evented_work_ready() const noexcept {
    const std::shared_ptr<const std::function<void()>> callback =
        impl_->evented_work_wake_callback_.load(std::memory_order_acquire);
    if (!callback || !*callback) {
        return;
    }
    try {
        (*callback)();
    } catch (const std::exception& error) {
        log_gui_error("event wake", error.what());
    } catch (...) {
        log_gui_error("event wake", "unknown event wake failure");
    }
}

std::function<void()> App::live_workspace_ready_callback() const {
    const std::shared_ptr<const std::function<void()>> callback =
        impl_->evented_work_wake_callback_.load(std::memory_order_acquire);
    if (!callback || !*callback) {
        return {};
    }
    return [this]() noexcept { notify_evented_work_ready(); };
}

void App::refresh_live_workspace_ready_callbacks() {
#if MMLTK_RFDETR_LIVE_CAPTURE
    std::function<void()> callback = live_workspace_ready_callback();
    if (live_controller_) {
        live_controller_->set_workspace_ready_callback(callback);
    }
    if (mmltk::live::LiveSessionController* controller = live_predict_controller_.controller(); controller != nullptr) {
        controller->set_workspace_ready_callback(std::move(callback));
    }
#endif
}

std::optional<mmltk::browser::WorkspaceSurfaceInfo> App::acquire_workspace_surface(
    const std::string_view surface_id) const {
    if (browser_publication_state_ == nullptr) {
        return std::nullopt;
    }
    return browser_publication_state_->workspace_surface_bridge.acquire_current_surface(surface_id);
}

std::optional<RetainedBrowserImportedFrameSource> App::acquire_workspace_surface_source(
    const std::string_view surface_id, const std::string_view revision) const {
    if (browser_publication_state_ == nullptr) {
        return std::nullopt;
    }
    return browser_publication_state_->workspace_surface_bridge.acquire_surface_source(surface_id, revision);
}

bool App::release_workspace_surface(const std::string_view surface_id, const std::string_view revision) {
    if (browser_publication_state_ == nullptr) {
        return false;
    }
    return browser_publication_state_->workspace_surface_bridge.release_surface(surface_id, revision);
}

void App::release_all_workspace_surface_publications() {
    release_all_browser_frame_publications();
}

void App::release_all_browser_frame_publications() {
    if (browser_publication_state_ == nullptr) {
        return;
    }

    release_browser_live_frame_slots();
    BrowserPublicationState& published = *browser_publication_state_;
    published.workspace_surface_bridge.reset();
}

void App::shutdown() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    set_evented_work_wake_callback({});
    browser_workspace_viewport_state_.reset();
    release_all_browser_frame_publications();
    stop_live_predict_session();
    annotation_workflow_coordinator_->shutdown();
    local_train_controller_.shutdown();
    settings_persistence_.flush();
    background_executor_.wait_idle();
    ui_callbacks_.drain();
}

std::optional<BrowserWorkspaceViewportState> App::browser_workspace_viewport_state() const {
    return browser_workspace_viewport_state_;
}

GuiSettingsSnapshot App::make_gui_settings_snapshot() {
    return GuiSettingsSnapshot{
        current_view_, selected_preset_name_, &ui_settings_, &train_, &validate_, &predict_, &annotate_, &export_};
}

mmltk::browser::StateSnapshot App::browser_state_snapshot() {
    GuiSettingsSnapshot gui_snapshot = make_gui_settings_snapshot();
    const LocalTrainSessionState& local_train_state = local_train_controller_.session_state();
    const RemoteTrainSessionState& remote_train_state = remote_train_controller_.state();
    std::string browser_preview_error;
    const bool static_preview_visible = !live_predict_running() && browser_publication_state_ != nullptr &&
                                        browser_publication_state_->predict_preview_source.has_value() &&
                                        !live_static_preview_source_name_.empty();

#if MMLTK_RFDETR_LIVE_CAPTURE
    mmltk::live::LiveSessionController* browser_live_controller = active_browser_live_controller();
    refresh_browser_live_frame_publications(browser_live_controller, &browser_preview_error);
#endif

    refresh_browser_retained_frame_publications(static_preview_visible);
    const BrowserPreviewSnapshot preview_state = [this, &browser_preview_error]() {
        BrowserPreviewSnapshot preview;
        preview.initialized = true;
        if (browser_publication_state_ == nullptr) {
            preview.last_error = browser_preview_error;
            return preview;
        }

        const BrowserPublicationState& published = *browser_publication_state_;
        const BrowserWorkspaceSurfaceBridge::PreviewRuntimeState& selected_preview =
            published.workspace_surface_bridge.preview();
        if (!selected_preview.has_frame) {
            preview.last_error = browser_preview_error;
            return preview;
        }

        preview.has_frame = true;
        preview.displayed_region = {
            selected_preview.displayed_region.x,
            selected_preview.displayed_region.y,
            selected_preview.displayed_region.width,
            selected_preview.displayed_region.height,
        };
        preview.last_frame_id = selected_preview.frame_id;
        preview.live_frame_id = selected_preview.live_frame_id;
        return preview;
    }();

    mmltk::browser::GuiStateSnapshotInputs inputs;
    inputs.gui = &gui_snapshot;
    if (should_surface_remote_train_job(remote_train_state, job_.running, current_view_)) {
        inputs.job = browser_job_state_from_remote_train(remote_train_state);
    } else if (should_surface_local_train_job(local_train_state, job_.running, current_view_)) {
        inputs.job = browser_job_state_from_local_train(local_train_state);
    } else {
        inputs.job.running = job_.running;
        inputs.job.label = job_.label;
        inputs.job.summary = job_.last_summary;
        inputs.job.error = job_.last_error;
        inputs.job.output_tail = snapshot_job_output();
    }
    const AnnotationFrame* annotation_frame = optional_value_ptr(annotate_frame_);
    inputs.annotation = mmltk::browser::annotation_document_state_from_editor(annotation_frame, &annotate_document_,
                                                                              &annotate_session_);
    if (browser_publication_state_ != nullptr) {
        inputs.workspace_surface = browser_publication_state_->workspace_surface_bridge.surface_info();
    }
    mmltk::browser::StateSnapshot snapshot = mmltk::browser::make_state_snapshot(inputs);
    {
        std::string resolved_preset_name = selected_preset_name_;
        const mmltk::rfdetr::ModelPresetConfig& active_preset = resolve_selected_preset(resolved_preset_name);
        const auto* active_module = model_registry_.find_module_for_preset(resolved_preset_name);
        nlohmann::json preset_options = nlohmann::json::array();
        for (const auto& preset : model_registry_.presets()) {
            preset_options.push_back({
                {"module_id", preset.module_id},
                {"preset_name", preset.preset_name},
                {"display_name", preset.display_name},
                {"selected", preset.preset_name == resolved_preset_name},
            });
        }
        snapshot.workflow_state["model_config"] = {
            {"selected_preset", resolved_preset_name},
            {"module_id", active_module != nullptr ? std::string(active_module->module_id()) : std::string{}},
            {"module_label", active_module != nullptr ? std::string(active_module->display_name()) : std::string{}},
            {"canonical_weight_filename", std::string(active_preset.canonical_weight_filename)},
            {"resolution", active_preset.resolution},
            {"num_queries", active_preset.num_queries},
            {"max_dets", active_preset.num_select},
            {"presets", std::move(preset_options)},
        };
    }
    snapshot.workflow_state["file_dialog"] = {
        {"active", active_file_browse_field_ != FileBrowseField::None},
        {"dialog_id", active_browser_dialog_id_},
        {"title", active_browser_dialog_title_},
        {"error", picker_error_},
    };
    if (const auto* viewport_state = optional_value_ptr(browser_workspace_viewport_state_); viewport_state != nullptr) {
        const auto* clip = optional_value_ptr(viewport_state->commit.clip);
        nlohmann::json workspace_viewport = {
            {"workflow", std::string(mmltk::browser::workflow_name(viewport_state->workflow))},
            {"center_x", viewport_state->commit.center_x},
            {"center_y", viewport_state->commit.center_y},
            {"zoom", viewport_state->commit.zoom},
            {"clip", clip != nullptr ? nlohmann::json(*clip) : nlohmann::json(nullptr)},
        };
        snapshot.workflow_state["workspace_viewport"] = std::move(workspace_viewport);
    }
    const AnnotationWorkflowPollPlan annotate_poll =
        plan_annotation_workflow_poll(annotation_workflow_coordinator_->make_save_request());
    const bool annotate_live_video = annotate_.source.kind == SourceKind::VideoStream;
    const bool annotate_block_actions =
        job_.running || live_predict_running() || annotate_assist_running_ || annotate_save_running_;
    const bool annotate_has_inputs = !annotate_inputs_.empty();
    const bool annotate_live_running_now = annotation_live_running();
    const AnnotationObject* selected_annotation_object =
        mmltk::gui::selected_object(annotate_document_, annotate_session_);
    const bool selected_supports_mask_editing =
        selected_annotation_object != nullptr && annotation_object_supports_mask_editing(*selected_annotation_object);
    const bool annotate_brush_enabled =
        selected_supports_mask_editing && annotation_tool_kind_uses_brush(annotate_session_.active_tool());
    snapshot.workflow_state["annotate_runtime"] = {
        {"setup",
         {{"live_video_source", annotate_live_video},
          {"live_capture_supported", live_capture_supported()},
          {"live_running", annotate_live_running_now},
          {"block_actions", annotate_block_actions},
          {"prepare_running", annotate_prepare_running_},
          {"frame_load_running", annotate_frame_load_running_},
          {"has_inputs", annotate_has_inputs},
          {"current_input_index",
           annotate_has_inputs ? nlohmann::json(annotate_current_input_index_) : nlohmann::json(nullptr)},
          {"input_count", annotate_inputs_.size()},
          {"can_start_live",
           annotate_live_video && !annotate_live_running_now && !annotate_block_actions && live_capture_supported()},
          {"can_stop_live", annotate_live_video && annotate_live_running_now},
          {"can_reload_frame", !annotate_live_video && annotate_has_inputs},
          {"can_prev_frame", !annotate_live_video && annotate_has_inputs && annotate_current_input_index_ > 0U},
          {"can_next_frame", !annotate_live_video && annotate_has_inputs &&
                                 annotate_current_input_index_ + 1U < annotate_inputs_.size()}}},
        {"hold_save",
         {{"enabled", annotate_poll.save_tab_plan.hold_save.enabled},
          {"active", annotate_poll.save_inputs.hold_save},
          {"blocked", annotate_poll.save_inputs.hold_save_blocked}}},
        {"brush",
         {{"radius", std::clamp(annotate_brush_radius_, 1, 128)},
          {"min_radius", 1},
          {"max_radius", 128},
          {"enabled", annotate_brush_enabled}}},
    };
    const std::string annotate_frame_summary =
        annotate_live_video ? "Setup-frame navigation is unavailable while video capture is "
                              "selected."
        : annotate_has_inputs
            ? std::format("Frame {} of {}", std::min(annotate_current_input_index_ + 1U, annotate_inputs_.size()),
                          annotate_inputs_.size())
            : "No setup-frame metadata has been published yet.";
    const std::string annotate_live_summary =
        !annotate_live_video        ? "Live annotate is only available for video-stream sources."
        : annotate_live_running_now ? "Live annotate is streaming from the current capture "
                                      "device."
        : live_capture_supported()  ? "Start live annotate to keep the workspace bound to "
                                      "incoming frames."
                                    : "Live annotate is unavailable in this build.";
    const bool annotate_save_now_enabled = annotate_poll.save_tab_plan.save_now.enabled;
    const bool annotate_hold_save_enabled = annotate_poll.save_tab_plan.hold_save.enabled;
    const std::string annotate_save_summary =
        annotate_poll.save_inputs.hold_save_blocked ? "Hold Save is blocked until the current queued frame drains."
        : annotate_poll.save_inputs.hold_save       ? "Hold Save is armed while the control stays pressed."
        : annotate_hold_save_enabled                ? "Hold Save captures frames continuously while live "
                                                      "annotate is running."
        : annotate_save_now_enabled                 ? "Save Now persists the current annotation "
                                                      "snapshot immediately."
                                                    : "Save controls enable after a writable frame is "
                                                      "available.";
    const std::string annotate_brush_summary = annotate_brush_enabled
                                                   ? "Brush radius stays in capture pixels and feeds the workspace "
                                                     "brush intent."
                                                   : "Brush radius appears when Paint or Erase is active on a "
                                                     "mask-capable selection.";
    snapshot.workflow_state["annotate_runtime_controls"] = {
        {"setup_frame_navigation",
         {{"visible", !annotate_live_video},
          {"current_index", annotate_has_inputs ? annotate_current_input_index_ : 0U},
          {"input_count", annotate_inputs_.size()},
          {"summary", annotate_frame_summary},
          {"reload",
           {{"enabled", !annotate_live_video && annotate_has_inputs},
            {"workflow", "annotate"},
            {"intent", "annotate.setup_frame.reload"},
            {"label", "Reload"}}},
          {"prev",
           {{"enabled", !annotate_live_video && annotate_has_inputs && annotate_current_input_index_ > 0U},
            {"workflow", "annotate"},
            {"intent", "annotate.setup_frame.prev"},
            {"label", "Prev"}}},
          {"next",
           {{"enabled", !annotate_live_video && annotate_has_inputs &&
                            annotate_current_input_index_ + 1U < annotate_inputs_.size()},
            {"workflow", "annotate"},
            {"intent", "annotate.setup_frame.next"},
            {"label", "Next"}}}}},
        {"live_annotate",
         {{"visible", annotate_live_video},
          {"running", annotate_live_running_now},
          {"summary", annotate_live_summary},
          {"start",
           {{"enabled",
             annotate_live_video && !annotate_live_running_now && !annotate_block_actions && live_capture_supported()},
            {"workflow", "annotate"},
            {"intent", "annotate.live.start"},
            {"label", "Start Live Annotate"}}},
          {"stop",
           {{"enabled", annotate_live_video && annotate_live_running_now},
            {"workflow", "annotate"},
            {"intent", "annotate.live.stop"},
            {"label", "Stop Live Annotate"}}}}},
        {"save",
         {{"visible", true},
          {"summary", annotate_save_summary},
          {"save_now",
           {{"enabled", annotate_save_now_enabled},
            {"workflow", "annotate"},
            {"intent", "annotate.save_now"},
            {"label", "Save Now"}}},
          {"hold_save",
           {{"enabled", annotate_poll.save_tab_plan.hold_save.enabled},
            {"blocked", annotate_poll.save_inputs.hold_save_blocked},
            {"value", annotate_poll.save_inputs.hold_save},
            {"workflow", "annotate"},
            {"intent", "annotate.hold_save"},
            {"label", "Hold Save"},
            {"payload_key", "active"}}}}},
        {"brush",
         {{"visible", annotate_brush_enabled},
          {"summary", annotate_brush_summary},
          {"radius",
           {{"value", std::clamp(annotate_brush_radius_, 1, 128)},
            {"min", 1},
            {"max", 128},
            {"enabled", annotate_brush_enabled},
            {"workflow", "annotate"},
            {"intent", "annotate.brush_radius"},
            {"label", "Brush Radius"},
            {"payload_key", "radius"}}}}},
    };
    snapshot.workflow_state["train_runtime"] = local_train_runtime_to_json(local_train_state);
    nlohmann::json& workflows = snapshot.workflow_state["workflows"];
    if (!workflows.is_object()) {
        workflows = nlohmann::json::object();
    }
    nlohmann::json& train_workflow = workflows["train"];
    if (!train_workflow.is_object()) {
        train_workflow = nlohmann::json::object();
    }
    train_workflow["local_gpu"] = mmltk::browser::train_local_gpu_state_to_json(
        local_train_controller_.gpus(), local_train_controller_.gpu_selection(), train_.local_device_ids,
        local_train_controller_.gpu_error(), local_train_controller_.gpu_refresh_running());
    train_workflow["remote_query"] = train_remote_query_to_json(vast_query_controller_, !vast_api_key_.empty());
    train_workflow["remote_session"] = remote_train_session_to_json(remote_train_state);
    const auto active_live_mode_name = [this]() -> std::string {
        switch (active_live_mode_) {
            case ActiveLiveMode::Predict:
                return "predict";
            case ActiveLiveMode::Annotate:
                return "annotate";
            case ActiveLiveMode::None:
                break;
        }
        return "none";
    };
    const bool live_video_source = active_live_mode_ == ActiveLiveMode::Annotate
                                       ? annotate_.source.kind == SourceKind::VideoStream
                                       : predict_.source.kind == SourceKind::VideoStream;
    mmltk::browser::BrowserLiveRuntimeState live_runtime;
    live_runtime.active_mode = active_live_mode_name();
    live_runtime.starting = live_predict_controller_.starting();
    live_runtime.stopping = live_predict_controller_.stopping();
    live_runtime.show_static_preview = static_preview_visible;
    live_runtime.show_idle_start_error = !live_predict_running() && !live_predict_controller_.start_error().empty();
    live_runtime.video_stream_source = live_video_source;
    live_runtime.runtime_capabilities = snapshot.runtime_capabilities;
    live_runtime.preview.initialized = preview_state.initialized;
    live_runtime.preview.has_frame = preview_state.has_frame;
    live_runtime.preview.displayed_region =
        mmltk::browser::capture_region_from_live_region(preview_state.displayed_region);
    live_runtime.preview.frame_id = preview_state.last_frame_id;
    live_runtime.preview.live_frame_id = preview_state.live_frame_id;
    live_runtime.preview.fit_to_capture = live_preview_fit_to_capture_;
    live_runtime.preview.crop_overlay_mode = live_crop_overlay_mode_;
    live_runtime.preview.static_source_name = live_static_preview_source_name_;
    live_runtime.preview.error = live_preview_error_.empty() ? preview_state.last_error : live_preview_error_;
    live_runtime.preview.interop_failed = preview_state.interop_failed;
    live_runtime.start_error = live_predict_controller_.start_error();
    live_runtime.action_error = live_predict_controller_.action_error();
#if MMLTK_RFDETR_LIVE_CAPTURE
    mmltk::live::LiveSessionController* active_live_controller = nullptr;
    if (active_live_mode_ == ActiveLiveMode::Annotate) {
        active_live_controller = live_controller_.get();
    } else {
        active_live_controller = live_predict_controller_.controller();
    }
    live_runtime.controller.present = active_live_controller != nullptr;
    std::optional<mmltk::live::LiveSessionStatus> active_live_status;
    if (active_live_mode_ == ActiveLiveMode::Annotate) {
        if (live_session_status_ != nullptr) {
            active_live_status = *live_session_status_;
        }
    } else if (const auto* status = live_predict_controller_.status(); status != nullptr) {
        active_live_status = *status;
    }
    if (!active_live_status.has_value() && active_live_controller != nullptr) {
        active_live_status = active_live_controller->snapshot_status();
    }
    live_runtime.show_running_section = active_live_mode_ == ActiveLiveMode::Predict && active_live_status.has_value();
    if (active_live_status.has_value()) {
        const mmltk::live::LiveSessionStatus& status = *active_live_status;
        live_runtime.controller.running = status.running;
        live_runtime.controller.last_error = status.last_error;
        live_runtime.analyzer.attached = status.analyzer.analyzer_attached;
        live_runtime.analyzer.model_hot = status.analyzer.model_hot;
        live_runtime.analyzer.running = status.analyzer.running;
        live_runtime.analyzer.frames_analyzed = status.analyzer.frames_analyzed;
        live_runtime.analyzer.frames_skipped = status.analyzer.frames_skipped;
        live_runtime.analyzer.last_latency_ms = status.analyzer.last_latency_ms;
        live_runtime.analyzer.backend_name = status.analyzer.backend_name;
        live_runtime.analyzer.last_error = status.analyzer.last_error;
        live_runtime.manual_overlay.running = status.manual.running;
        live_runtime.manual_overlay.generations_rendered = status.manual.generations_rendered;
        live_runtime.manual_overlay.last_generation = status.manual.last_generation;
        live_runtime.manual_overlay.last_error = status.manual.last_error;
        live_runtime.compositor.running = status.compositor.running;
        live_runtime.compositor.frames_composited = status.compositor.frames_composited;
        live_runtime.compositor.frames_dropped = status.compositor.frames_dropped;
        live_runtime.compositor.last_frame_id = status.compositor.last_frame_id;
        live_runtime.compositor.manual_overlay_active = status.compositor.manual_overlay_active;
        live_runtime.compositor.analysis_overlay_active = status.compositor.analysis_overlay_active;
        live_runtime.compositor.last_error = status.compositor.last_error;
    }
#endif
    nlohmann::json live_runtime_json = mmltk::browser::live_runtime_state_to_json(live_runtime);
    nlohmann::json& live_preview_json = live_runtime_json["preview"];
    if (!live_preview_json.is_object()) {
        live_preview_json = nlohmann::json::object();
    }
    const bool show_live_preview_controls =
        (active_live_mode_ == ActiveLiveMode::Predict && live_predict_controller_.controller() != nullptr) ||
        static_preview_visible;
    const bool can_toggle_live_full_frame = active_live_mode_ == ActiveLiveMode::Predict &&
                                            live_predict_controller_.controller() != nullptr &&
                                            predict_.source.kind == SourceKind::VideoStream;
    live_preview_json["full_frame"] = live_crop_overlay_mode_;
    live_preview_json["show_controls"] = show_live_preview_controls;
    live_preview_json["can_toggle_fit_to_capture"] = show_live_preview_controls;
    live_preview_json["can_toggle_full_frame"] = can_toggle_live_full_frame;
    snapshot.workflow_state["live_runtime"] = std::move(live_runtime_json);
    snapshot.workflow_state["live_preview_controls"] = {
        {"fit_to_capture",
         {{"value", live_preview_fit_to_capture_},
          {"enabled", show_live_preview_controls},
          {"workflow", "live"},
          {"intent", "live.preview.fit_to_capture"},
          {"label", "Fit To Capture"},
          {"payload_key", "enabled"}}},
        {"full_frame_display",
         {{"value", live_crop_overlay_mode_},
          {"enabled", can_toggle_live_full_frame},
          {"workflow", "live"},
          {"intent", "live.preview.full_frame_display"},
          {"label", "Full Frame"},
          {"payload_key", "enabled"}}},
    };
    snapshot.workflow_state["annotate_sidebar"] = annotation_sidebar_state_to_json(
        AnnotationSidebarModelBuilder::build(annotate_document_, annotate_categories_, annotate_session_,
                                             annotate_frame_.has_value()),
        annotate_frame_.has_value(), annotate_cleanup_radius_, annotate_.model_input != ModelInputMode::None,
        annotate_assist_running_, annotate_assist_summary_, annotate_assist_error_);
    if (const AnnotationFrame* annotate_workspace_frame = optional_value_ptr(annotate_frame_);
        annotate_workspace_frame != nullptr) {
        const std::shared_ptr<const AnnotationProjectedScene> projected_scene = resolve_annotation_projected_scene(
            annotate_workflow_, annotate_document_, annotate_session_, annotate_frame_);
        const AnnotationBrushPreview& brush_preview = annotate_session_.brush_preview();
        const std::vector<AnnotationResolvedObject>* resolved_objects =
            annotate_resolved_instances_.empty() ? nullptr : &annotate_resolved_instances_;
        snapshot.workflow_state["annotate_workspace_scene"] =
            projected_scene != nullptr ? nlohmann::json(annotation_workspace_scene_to_json(
                                             *annotate_workspace_frame, *projected_scene,
                                             brush_preview.visible ? &brush_preview : nullptr, resolved_objects))
                                       : nlohmann::json(nullptr);
    } else {
        snapshot.workflow_state["annotate_workspace_scene"] = nullptr;
    }
    return snapshot;
}

void App::apply_annotation_sidebar_mutation_result(const AnnotationSidebarMutationResult& result,
                                                   const bool allow_assist) {
    apply_annotation_sidebar_mutation_effects(
        result, allow_assist,
        [this]() {
            (void)launch_annotation_workflow_assist(
                AnnotationWorkflowAssistLaunchRequest{
                    annotation_frame_ptr(annotate_frame_),
                    annotate_assist_running_,
                    annotate_.model_input,
                },
                annotation_live_running(),
                AnnotationWorkflowAssistLaunchState{
                    &annotate_assist_running_,
                    &annotate_assist_summary_,
                    &annotate_assist_error_,
                },
                background_executor_, ui_callbacks_,
                [this](std::string* error_message) {
                    return annotation_workflow_coordinator_->ensure_live_annotation_frame_pixels(error_message);
                },
                [this]() -> std::optional<AnnotationWorkflowAssistExecutionRequest> {
                    return make_annotation_workflow_assist_execution_request(
                        *annotate_frame_, annotate_, selected_preset_name_, &annotate_assist_error_);
                },
                [this](const AnnotationWorkflowAssistOutcome& outcome) {
                    if (!annotation_workflow_assist_outcome_matches_frame(annotation_frame_ptr(annotate_frame_),
                                                                          outcome)) {
                        return;
                    }
                    const AnnotationWorkflowAssistApplyResult apply_result = apply_annotation_workflow_assist_outcome(
                        annotate_document_, annotate_session_, annotate_categories_, outcome);
                    if (!apply_result.changed) {
                        return;
                    }
                    cancel_annotation_canvas_interactions();
                    invalidate_annotation_preview();
                },
                [](const std::string& error) { log_gui_error("annotate assist error", error); });
        },
        [this]() { cancel_annotation_canvas_interactions(); },
        [this](const AnnotationToolKind tool) {
            (void)annotate_controller_.set_active_tool(tool, annotate_document_, annotate_session_);
        },
        [this]() { invalidate_annotation_preview(); });
}

void App::apply_browser_intent(const mmltk::browser::IntentMessage& intent) {
    const mmltk::browser::RoutedIntent routed = mmltk::browser::route_intent(intent);
    const auto make_gui_snapshot = [this]() { return make_gui_settings_snapshot(); };
    const auto apply_settings_patch = [this, &make_gui_snapshot](const nlohmann::json& patch) {
        if (!patch.is_object() || patch.empty()) {
            return;
        }
        if (const auto preset = patch.find("selected_preset"); preset != patch.end() && preset->is_string()) {
            const std::string requested_preset = preset->get<std::string>();
            if (requested_preset != selected_preset_name_) {
                selected_preset_name_ = requested_preset;
                apply_selected_preset_defaults();
            }
        }

        GuiSettingsSnapshot gui_snapshot = make_gui_snapshot();
        mmltk::browser::apply_settings_update(mmltk::browser::SettingsUpdateIntent{patch}, gui_snapshot);
        current_view_ = gui_snapshot.current_view;
        selected_preset_name_ = gui_snapshot.selected_preset;
        local_train_controller_.set_selected_device_ids(train_.local_device_ids);
    };
    const auto apply_routed_host_intent = [this, &routed, &make_gui_snapshot]() {
        GuiSettingsSnapshot gui_snapshot = make_gui_snapshot();
        mmltk::browser::apply_routed_intent(
            routed, mmltk::browser::BrowserHostIntentContext{
                        &current_view_,
                        &selected_preset_name_,
                        &gui_snapshot,
                        &predict_,
                        &annotate_,
                        &annotate_document_,
                        &annotate_session_,
                        &annotate_controller_,
                        annotation_frame_ptr(annotate_frame_),
                        &annotate_categories_,
                        [this](std::string* error_message) {
                            return annotation_workflow_coordinator_->ensure_live_annotation_frame_pixels(error_message);
                        },
                    });
    };

    if (const auto* settings = std::get_if<mmltk::browser::SettingsUpdateIntent>(&routed.payload);
        settings != nullptr) {
        apply_settings_patch(settings->patch);
        return;
    }

    if (const auto* crop = std::get_if<mmltk::browser::CropCommitIntent>(&routed.payload); crop != nullptr) {
        apply_routed_host_intent();
        if ((routed.workflow == mmltk::browser::Workflow::Predict ||
             routed.workflow == mmltk::browser::Workflow::Live) &&
            live_predict_controller_.controller() != nullptr) {
            const AnnotationBox crop_box =
          crop->has_crop
              ? AnnotationBox{
                    static_cast<int>(crop->crop.x),
                    static_cast<int>(crop->crop.y),
                    static_cast<int>(crop->crop.x + crop->crop.width),
                    static_cast<int>(crop->crop.y + crop->crop.height),
                }
              : full_capture_box_for_source(predict_.source);
            publish_runtime_crop_box(live_predict_controller_.controller()->ui_crop_state(), predict_.source, crop_box);
        }
        if (routed.workflow == mmltk::browser::Workflow::Annotate) {
            cancel_annotation_canvas_interactions();
            invalidate_annotation_preview();
        }
        return;
    }

    if (std::holds_alternative<mmltk::browser::ToolSelectIntent>(routed.payload)) {
        apply_routed_host_intent();
        cancel_annotation_canvas_interactions();
        return;
    }

    if (const auto* live_preview_controls = std::get_if<mmltk::browser::LivePreviewControlIntent>(&routed.payload);
        live_preview_controls != nullptr) {
        if (live_preview_controls->fit_to_capture.has_value()) {
            live_preview_fit_to_capture_ = live_preview_controls->fit_to_capture.value();
        }
        if (live_preview_controls->crop_overlay_mode.has_value()) {
            const bool can_toggle_full_frame = active_live_mode_ == ActiveLiveMode::Predict &&
                                               live_predict_controller_.controller() != nullptr &&
                                               predict_.source.kind == SourceKind::VideoStream;
            const bool next_crop_overlay_mode = live_preview_controls->crop_overlay_mode.value();
            if (next_crop_overlay_mode && !can_toggle_full_frame) {
                throw std::runtime_error(
                    "browser live.preview.update full_frame requires a running live "
                    "video preview");
            }
            if (live_crop_overlay_mode_ != next_crop_overlay_mode) {
                live_crop_overlay_mode_ = next_crop_overlay_mode;
                live_crop_drag_session_ = {};
            }
        }
        return;
    }

    if (std::holds_alternative<mmltk::browser::AnnotateWorkspaceClickIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceBoxDragIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceHandleDragIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceBrushIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceFillIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceColorSampleIntent>(routed.payload)) {
        apply_routed_host_intent();
        invalidate_annotation_preview();
        return;
    }

    if (std::holds_alternative<mmltk::browser::TrainLocalGpuRefreshIntent>(routed.payload)) {
        current_view_ = View::Train;
        local_train_controller_.refresh_visible_gpus(train_.local_device_ids);
        return;
    }

    if (const auto* train_start = std::get_if<mmltk::browser::TrainStartIntent>(&routed.payload);
        train_start != nullptr) {
        apply_settings_patch(train_start->options);
        current_view_ = View::Train;
        if (train_.execution_target != TrainExecutionTarget::Local) {
            throw std::runtime_error("browser train.start is only available for the local training path");
        }
        if (job_.running) {
            throw std::runtime_error("browser train.start blocked while another job is running");
        }
        if (live_predict_running()) {
            throw std::runtime_error("browser train.start blocked while live predict is running");
        }
        if (local_train_controller_.running()) {
            throw std::runtime_error(
                "browser train.start blocked while local "
                "training is already running");
        }
        if (!local_train_controller_.gpu_error().empty()) {
            throw std::runtime_error("browser train.start blocked: " + local_train_controller_.gpu_error());
        }
        if (local_train_controller_.selected_device_ids().empty()) {
            throw std::runtime_error(
                "browser train.start blocked until at least one "
                "local GPU is selected");
        }
        local_train_controller_.start(train_, selected_preset_name_);
        return;
    }

    if (const auto* train_stop = std::get_if<mmltk::browser::TrainStopIntent>(&routed.payload); train_stop != nullptr) {
        current_view_ = View::Train;
        local_train_controller_.request_stop(train_stop->force);
        return;
    }

    if (std::holds_alternative<mmltk::browser::TrainRemoteQueryIntent>(routed.payload)) {
        current_view_ = View::Train;
        if (train_.execution_target != TrainExecutionTarget::Remote) {
            throw std::runtime_error("browser train.remote.query requires the remote train target");
        }
        vast_query_controller_.launch(vast_api_key_, selected_remote_gpu_families());
        return;
    }

    if (const auto* remote_offer_arm = std::get_if<mmltk::browser::TrainRemoteOfferArmIntent>(&routed.payload);
        remote_offer_arm != nullptr) {
        current_view_ = View::Train;
        if (train_.execution_target != TrainExecutionTarget::Remote) {
            throw std::runtime_error("browser train.remote.offer.arm requires the remote train target");
        }
        for (const VastOfferSummary& offer : vast_query_controller_.state().results) {
            if (offer.offer_id == remote_offer_arm->offer_id) {
                vast_query_controller_.arm_offer(offer);
                return;
            }
        }
        throw std::runtime_error("browser train.remote.offer.arm could not find offer: " +
                                 std::to_string(remote_offer_arm->offer_id));
    }

    if (std::holds_alternative<mmltk::browser::TrainRemoteOfferClearIntent>(routed.payload)) {
        current_view_ = View::Train;
        vast_query_controller_.clear_armed_offer();
        return;
    }

    if (const auto* remote_start = std::get_if<mmltk::browser::TrainRemoteStartIntent>(&routed.payload);
        remote_start != nullptr) {
        apply_settings_patch(remote_start->options);
        current_view_ = View::Train;
        if (train_.execution_target != TrainExecutionTarget::Remote) {
            throw std::runtime_error("browser train.remote.start requires the remote train target");
        }
        if (job_.running) {
            throw std::runtime_error("browser train.remote.start blocked while another job is running");
        }
        if (live_predict_running()) {
            throw std::runtime_error("browser train.remote.start blocked while live predict is running");
        }
        if (local_train_controller_.running()) {
            throw std::runtime_error(
                "browser train.remote.start blocked while local "
                "training is already running");
        }
        if (vast_api_key_.empty()) {
            throw std::runtime_error("browser train.remote.start requires --vast-api-key or VAST_API_KEY");
        }

        const RemoteTrainSessionState& remote_state = remote_train_controller_.state();
        if (remote_state.request_running || remote_state.refresh_running) {
            throw std::runtime_error(
                "browser train.remote.start blocked while the "
                "remote session is busy");
        }

        if (remote_state.instance.has_value()) {
            if (remote_train_instance_running(*remote_state.instance)) {
                throw std::runtime_error(
                    "browser train.remote.start blocked while the "
                    "remote instance is already running");
            }
            remote_train_controller_.start(vast_api_key_);
            return;
        }

        const auto& armed_offer = vast_query_controller_.armed_offer();
        if (!armed_offer.has_value()) {
            throw std::runtime_error("browser train.remote.start requires an armed remote offer");
        }
        if (train_.remote_container_image.empty()) {
            throw std::runtime_error("browser train.remote.start requires a remote container image");
        }
        if (train_.remote_launch_template.empty()) {
            throw std::runtime_error("browser train.remote.start requires a remote launch template");
        }
        remote_train_controller_.launch(vast_api_key_, *armed_offer, train_.remote_container_image,
                                        train_.remote_launch_template);
        return;
    }

    if (std::holds_alternative<mmltk::browser::TrainRemoteStopIntent>(routed.payload)) {
        current_view_ = View::Train;
        if (train_.execution_target != TrainExecutionTarget::Remote) {
            throw std::runtime_error("browser train.remote.stop requires the remote train target");
        }
        if (vast_api_key_.empty()) {
            throw std::runtime_error("browser train.remote.stop requires --vast-api-key or VAST_API_KEY");
        }

        const RemoteTrainSessionState& remote_state = remote_train_controller_.state();
        if (!remote_state.instance.has_value()) {
            throw std::runtime_error("browser train.remote.stop requires an existing remote instance");
        }
        if (remote_state.request_running || remote_state.refresh_running) {
            throw std::runtime_error("browser train.remote.stop blocked while the remote session is busy");
        }
        if (!remote_train_instance_running(*remote_state.instance)) {
            throw std::runtime_error("browser train.remote.stop requires a running remote instance");
        }
        remote_train_controller_.stop(vast_api_key_);
        return;
    }

    if (std::holds_alternative<mmltk::browser::PredictStopIntent>(routed.payload)) {
        if (active_live_mode_ != ActiveLiveMode::Predict && !live_predict_controller_.starting() &&
            !live_predict_controller_.stopping()) {
            throw std::runtime_error("browser predict.stop requires an active live predict session");
        }
        stop_live_predict_session();
        return;
    }

    if (const auto* predict_start = std::get_if<mmltk::browser::PredictStartIntent>(&routed.payload);
        predict_start != nullptr) {
        apply_settings_patch(mmltk::browser::make_predict_start_settings_patch(*predict_start));

        const PredictUiState ui_state = evaluate_predict_ui_state(
            predict_, job_.running, job_.label, live_predict_running(), active_live_mode_ == ActiveLiveMode::Annotate);
        const bool wants_live =
            routed.workflow == mmltk::browser::Workflow::Live || predict_.source.kind == SourceKind::VideoStream;
        if (wants_live) {
            if (!ui_state.live_blocker.empty()) {
                throw std::runtime_error("browser predict.start blocked: " + ui_state.live_blocker);
            }
            if (active_live_mode_ == ActiveLiveMode::Annotate) {
                throw std::runtime_error("browser predict.start blocked while live annotate is active");
            }
            current_view_ = View::Live;
            launch_live_predict_session();
            return;
        }

        if (!ui_state.source_error.empty()) {
            throw std::runtime_error("browser predict.start invalid source: " + ui_state.source_error);
        }
        if (!ui_state.combo_error.empty()) {
            throw std::runtime_error("browser predict.start invalid predict configuration: " + ui_state.combo_error);
        }
        if (job_.running) {
            throw std::runtime_error("browser predict.start blocked while another job is running");
        }
        if (live_predict_running()) {
            throw std::runtime_error("browser predict.start blocked while live predict is running");
        }

        current_view_ = View::Predict;
        clear_static_preview();
        launch_predict_job();
        return;
    }

    if (const auto* validate_start = std::get_if<mmltk::browser::ValidateStartIntent>(&routed.payload);
        validate_start != nullptr) {
        apply_settings_patch(validate_start->options);
        current_view_ = View::Validate;
        launch_validate_job();
        return;
    }

    if (const auto* export_start = std::get_if<mmltk::browser::ExportStartIntent>(&routed.payload);
        export_start != nullptr) {
        apply_settings_patch(export_start->options);
        current_view_ = View::Export;
        launch_export_job();
        return;
    }

    if (const auto* annotate_save = std::get_if<mmltk::browser::AnnotateSaveIntent>(&routed.payload);
        annotate_save != nullptr) {
        apply_settings_patch(mmltk::browser::make_annotate_save_settings_patch(*annotate_save));
        current_view_ = View::Annotate;
        if (!request_annotation_save_now()) {
            throw std::runtime_error(
                "browser annotate.save is not available for the "
                "current annotation state");
        }
        return;
    }

    if (const auto* annotate_setup = std::get_if<mmltk::browser::AnnotateSetupIntent>(&routed.payload);
        annotate_setup != nullptr) {
        current_view_ = View::Annotate;
        const bool live_video = annotate_.source.kind == SourceKind::VideoStream;
        const bool block_actions =
            job_.running || live_predict_running() || annotate_assist_running_ || annotate_save_running_;
        switch (annotate_setup->action) {
            case mmltk::browser::AnnotateSetupAction::StartLive:
                if (!live_video) {
                    throw std::runtime_error(
                        "browser annotate.setup start_live requires a video-stream "
                        "annotate source");
                }
                if (!live_capture_supported()) {
                    throw std::runtime_error(
                        "browser annotate.setup start_live is not available in this "
                        "build");
                }
                if (annotation_live_running()) {
                    throw std::runtime_error("browser annotate.setup start_live is already active");
                }
                if (block_actions) {
                    throw std::runtime_error(
                        "browser annotate.setup start_live is blocked while annotate "
                        "shell actions are busy");
                }
                annotation_workflow_coordinator_->start_live_session();
                if (!annotation_live_running()) {
                    throw std::runtime_error(annotate_save_error_.empty()
                                                 ? "browser annotate.setup start_live failed"
                                                 : "browser annotate.setup start_live failed: " + annotate_save_error_);
                }
                return;
            case mmltk::browser::AnnotateSetupAction::StopLive:
                if (!annotation_live_running()) {
                    throw std::runtime_error("browser annotate.setup stop_live is not active");
                }
                annotation_workflow_coordinator_->stop_live_session();
                return;
            case mmltk::browser::AnnotateSetupAction::ReloadFrame:
                if (live_video) {
                    throw std::runtime_error(
                        "browser annotate.setup reload_frame is unavailable for live "
                        "annotate sources");
                }
                if (annotate_inputs_.empty()) {
                    throw std::runtime_error(
                        "browser annotate.setup reload_frame requires a prepared "
                        "annotate source");
                }
                annotation_workflow_coordinator_->load_current_frame();
                return;
            case mmltk::browser::AnnotateSetupAction::PrevFrame:
                if (live_video) {
                    throw std::runtime_error(
                        "browser annotate.setup prev_frame is unavailable for live "
                        "annotate sources");
                }
                if (annotate_inputs_.empty()) {
                    throw std::runtime_error(
                        "browser annotate.setup prev_frame requires a prepared annotate "
                        "source");
                }
                if (annotate_current_input_index_ > 0U) {
                    annotation_workflow_coordinator_->step_current_frame(-1);
                }
                return;
            case mmltk::browser::AnnotateSetupAction::NextFrame:
                if (live_video) {
                    throw std::runtime_error(
                        "browser annotate.setup next_frame is unavailable for live "
                        "annotate sources");
                }
                if (annotate_inputs_.empty()) {
                    throw std::runtime_error(
                        "browser annotate.setup next_frame requires a prepared annotate "
                        "source");
                }
                if (annotate_current_input_index_ + 1U < annotate_inputs_.size()) {
                    annotation_workflow_coordinator_->step_current_frame(1);
                }
                return;
        }
    }

    if (const auto* annotate_hold_save = std::get_if<mmltk::browser::AnnotateHoldSaveIntent>(&routed.payload);
        annotate_hold_save != nullptr) {
        current_view_ = View::Annotate;
        if (annotate_hold_save->enabled) {
            const AnnotationWorkflowPollPlan annotate_poll =
                plan_annotation_workflow_poll(annotation_workflow_coordinator_->make_save_request());
            if (!annotate_poll.save_tab_plan.hold_save.enabled) {
                throw std::runtime_error(
                    "browser annotate.hold_save is only available while live "
                    "annotate is running with a writable frame");
            }
        }
        annotate_workflow_.save_controller.hold_save = annotate_hold_save->enabled;
        annotate_workflow_.save_controller.hold_save_blocked = false;
        return;
    }

    if (const auto* annotate_brush_radius = std::get_if<mmltk::browser::AnnotateBrushRadiusIntent>(&routed.payload);
        annotate_brush_radius != nullptr) {
        current_view_ = View::Annotate;
        annotate_brush_radius_ = std::clamp(annotate_brush_radius->radius, 1, 128);
        AnnotationBrushPreview brush_preview = annotate_session_.brush_preview();
        if (brush_preview.visible) {
            brush_preview.radius = annotate_brush_radius_;
            annotate_session_.set_brush_preview(brush_preview);
        }
        return;
    }

    if (const auto* annotate_sidebar = std::get_if<mmltk::browser::AnnotateSidebarIntent>(&routed.payload);
        annotate_sidebar != nullptr) {
        current_view_ = View::Annotate;
        const AnnotationSidebarViewModel sidebar_model = AnnotationSidebarModelBuilder::build(
            annotate_document_, annotate_categories_, annotate_session_, annotate_frame_.has_value());

        const AnnotationFrame* annotation_frame = optional_value_ptr(annotate_frame_);
        AnnotationSelectedObjectSidebarActionPayload request;
        AnnotationMaskSidebarActionPayload mask_request;
        bool use_mask_request = false;
        if (annotate_sidebar->action == "select_object") {
            if (annotate_sidebar->object_index.has_value()) {
                request.selected_object_index = static_cast<std::size_t>(*annotate_sidebar->object_index);
            } else {
                request.selected_object_index = std::nullopt;
            }
        } else if (annotate_sidebar->action == "add_category") {
            annotate_pending_class_name_ = annotate_sidebar->category_name;
            request.request_add_class = true;
        } else if (annotate_sidebar->action == "create_object") {
            request.create_object_tool = mmltk::browser::annotation_tool_kind_from_name(annotate_sidebar->tool);
        } else if (annotate_sidebar->action == "assist") {
            request.request_assist = true;
        } else if (annotate_sidebar->action == "delete_selected") {
            request.request_delete_selected = true;
        } else if (annotate_sidebar->action == "undo") {
            request.request_undo = true;
        } else if (annotate_sidebar->action == "redo") {
            request.request_redo = true;
        } else if (annotate_sidebar->action == "update_selected") {
            request.update_selected_metadata = true;
            request.selected_enabled = annotate_sidebar->enabled.value_or(
                sidebar_model.selected_object.has_value() ? sidebar_model.selected_object->enabled : true);
            request.selected_category_index =
                annotate_sidebar->category_index.has_value()
                    ? static_cast<std::size_t>(*annotate_sidebar->category_index)
                    : (sidebar_model.selected_object.has_value() ? sidebar_model.selected_object->category_index : 0U);
        } else if (annotate_sidebar->action == "redraw_box") {
            request.request_redraw_box = true;
        } else if (annotate_sidebar->action == "spline.insert_active_knot") {
            request.request_insert_active_spline_knot = true;
        } else if (annotate_sidebar->action == "spline.update_active_segment") {
            request.update_spline_active_segment = true;
            request.spline_active_segment_index =
                annotate_sidebar->spline_segment_index.has_value()
                    ? std::optional<std::size_t>(static_cast<std::size_t>(*annotate_sidebar->spline_segment_index))
                    : std::nullopt;
        } else if (annotate_sidebar->action == "spline.set_handle_mode") {
            request.spline_handle_mode = annotation_spline_handle_mode_from_name(annotate_sidebar->spline_handle_mode);
        } else if (annotate_sidebar->action == "spline.close") {
            request.spline_action = AnnotationToolActionKind::Confirm;
        } else if (annotate_sidebar->action == "spline.reopen") {
            request.spline_action = AnnotationToolActionKind::ReopenSpline;
        } else if (annotate_sidebar->action == "spline.delete_active_knot") {
            request.spline_action = AnnotationToolActionKind::DeleteActiveElement;
        } else if (annotate_sidebar->action == "skeleton.update_active_joint") {
            request.update_skeleton_active_joint = true;
            request.skeleton_active_joint_index =
                annotate_sidebar->skeleton_joint_index.has_value()
                    ? std::optional<std::size_t>(static_cast<std::size_t>(*annotate_sidebar->skeleton_joint_index))
                    : std::nullopt;
        } else if (annotate_sidebar->action == "skeleton.skip_joint") {
            request.skeleton_action = AnnotationToolActionKind::SkipJoint;
        } else if (annotate_sidebar->action == "skeleton.hide_joint") {
            request.skeleton_action = AnnotationToolActionKind::HideJoint;
        } else if (annotate_sidebar->action == "skeleton.show_joint") {
            request.skeleton_action = AnnotationToolActionKind::ReactivateJoint;
        } else if (annotate_sidebar->action == "skeleton.reseed_joint") {
            request.skeleton_action = AnnotationToolActionKind::ReseedJoint;
        } else if (annotate_sidebar->action == "mask.cleanup") {
            use_mask_request = true;
            mask_request.cleanup_radius = annotate_sidebar->cleanup_radius.value_or(annotate_cleanup_radius_);
            mask_request.cleanup_op =
                mmltk::browser::annotation_mask_cleanup_op_from_browser_name(annotate_sidebar->cleanup_op);
        } else if (annotate_sidebar->action == "mask.update_color_ranges") {
            use_mask_request = true;
            const AnnotationColorRange fallback_sup =
                sidebar_model.selected_object.has_value() ? sidebar_model.selected_object->sup : AnnotationColorRange{};
            const AnnotationColorRange fallback_nosup = sidebar_model.selected_object.has_value()
                                                            ? sidebar_model.selected_object->nosup
                                                            : AnnotationColorRange{};
            mask_request.cleanup_radius = annotate_cleanup_radius_;
            mask_request.update_color_ranges = true;
            mask_request.sup = annotation_color_range_from_json(annotate_sidebar->sup, fallback_sup);
            mask_request.nosup = annotation_color_range_from_json(annotate_sidebar->nosup, fallback_nosup);
        } else {
            throw std::runtime_error("unsupported browser annotate.sidebar action: " + annotate_sidebar->action);
        }

        const AnnotationSidebarMutationResult result =
            use_mask_request ? apply_annotation_mask_sidebar_edit(
                                   AnnotationMaskTabApplyContext{
                                       annotate_document_,
                                       annotate_session_,
                                       annotation_frame,
                                       &annotate_cleanup_radius_,
                                   },
                                   mask_request)
                             : apply_annotation_selected_object_sidebar_payload(
                                   AnnotationObjectsTabApplyContext{
                                       annotate_controller_,
                                       annotate_document_,
                                       annotate_session_,
                                       annotate_categories_,
                                       annotation_frame,
                                       &annotate_pending_class_name_,
                                       annotate_.model_input != ModelInputMode::None,
                                       annotate_sidebar->category_index.has_value()
                                           ? static_cast<std::size_t>(*annotate_sidebar->category_index)
                                           : sidebar_model.preferred_new_object_category_index,
                                   },
                                   request);
        apply_annotation_sidebar_mutation_result(result, !use_mask_request);
        return;
    }

    if (const auto* file_dialog = std::get_if<mmltk::browser::FileDialogRequestIntent>(&routed.payload);
        file_dialog != nullptr) {
        launch_browser_file_dialog(*file_dialog);
        return;
    }

    if (const auto* viewport = std::get_if<mmltk::browser::ViewportCommitIntent>(&routed.payload);
        viewport != nullptr) {
        queue_browser_viewport_commit(routed.workflow, *viewport);
        return;
    }
}

void App::drain_background_work() {
    ui_callbacks_.drain(kMaxUiCallbacksPerPoll);
    local_train_controller_.poll();
    remote_train_controller_.poll(vast_api_key_);
    live_predict_controller_.poll_status();

    const auto now = std::chrono::steady_clock::now();
    if (now >= impl_->next_settings_persistence_poll_) {
        GuiSettingsSnapshot snap = make_gui_settings_snapshot();
        settings_persistence_.notify_frame(snap);
        impl_->next_settings_persistence_poll_ = now + kSettingsPersistencePollInterval;
    }
#if MMLTK_RFDETR_LIVE_CAPTURE
    mmltk::live::LiveSessionController* active_live_controller = nullptr;
    if (active_live_mode_ == ActiveLiveMode::Annotate) {
        active_live_controller = live_controller_.get();
    } else if (active_live_mode_ == ActiveLiveMode::Predict) {
        active_live_controller = live_predict_controller_.controller();
    }
    if (active_live_controller != nullptr) {
        const std::string controller_error = active_live_controller->last_error();
        if (!controller_error.empty()) {
            live_preview_error_ = controller_error;
        }
    }
#endif
    poll_annotate_work();

    if (live_controller_) {
        if (!live_session_status_) {
            live_session_status_ = std::make_unique<mmltk::live::LiveSessionStatus>();
        }
        *live_session_status_ = live_controller_->snapshot_status();
    }
}

void App::launch_job(const std::string& label, std::function<JobOutcome()> fn) {
    if (job_.running) {
        return;
    }
    job_.running = true;
    job_.label = label;
    job_.last_summary.clear();
    job_.last_error.clear();
    {
        std::lock_guard<std::mutex> lock(job_.output_mutex);
        job_.output_tail.clear();
    }
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [this, task = std::move(fn)]() mutable {
            rfdetr::ScopedTensorRtLogSink trt_log_sink([this](const std::string& line) { append_job_output(line); });
            return task();
        },
        [this](JobOutcome outcome) mutable {
            job_.last_summary = std::move(outcome.summary);
            job_.last_error.clear();
            if (outcome.preview.has_value()) {
                apply_static_preview(std::move(*outcome.preview));
                current_view_ = View::Live;
            }
            job_.running = false;
        },
        [this](const std::string& error) {
            job_.last_error = error;
            job_.last_summary.clear();
            job_.running = false;
            log_gui_error("job error", job_.last_error);
        });
}

void App::launch_validate_job() {
    if (job_.running) {
        throw std::runtime_error("validate is blocked while another job is running");
    }
    if (live_predict_running()) {
        throw std::runtime_error("validate is blocked while live predict is running");
    }

    const ValidateViewState state = validate_;
    launch_job("validate", [state]() {
        const mmltk::rfdetr::ValidateRequest request = rfdetr_workflows::build_validate_request(state);
        const ValidationOptions options = mmltk::rfdetr::to_validate_options(request);
        const ValidationRunResult result = run_validation(options);
        print_validation_run_summary(options, result);
        JobOutcome outcome;
        outcome.summary = rfdetr_workflows::summarize_validation_result(result);
        return outcome;
    });
}

void App::launch_predict_job() {
    const PredictViewState state = predict_;
    const std::string preset_name = selected_preset_name_;
    launch_job("predict", [state, preset_name]() {
        PreparedPredictSource prepared_source = prepare_predict_source(state.source);
        const mmltk::rfdetr::PredictRequest request =
            rfdetr_workflows::build_predict_request(state, std::move(prepared_source.image_inputs));
        PredictOptions options = mmltk::rfdetr::to_predict_options(request, preset_name);
        const PredictionRunResult result = run_prediction(options);
        write_prediction_json(options, result);
        print_prediction_summary(options, result);
        JobOutcome outcome;
        outcome.summary = rfdetr_workflows::summarize_prediction_result(result);
        outcome.preview = rfdetr_workflows::maybe_make_single_image_preview(state, options, result);
        return outcome;
    });
}

void App::launch_export_job() {
    if (job_.running) {
        throw std::runtime_error("export is blocked while another job is running");
    }
    if (live_predict_running()) {
        throw std::runtime_error("export is blocked while live predict is running");
    }

    const ExportViewState state = export_;
    launch_job("export", [this, state]() {
        append_job_output("[export] start");
        std::filesystem::path onnx_path = state.onnx_path;

        if (!state.weights_path.empty()) {
            const ExportOnnxRequest export_request = rfdetr_workflows::build_export_onnx_request(state, onnx_path);
            append_job_output("[export] writing ONNX: " + export_request.output_path.string());
            rfdetr::export_weights_to_onnx(export_request.weights_path, export_request.output_path,
                                           export_request.device_id, export_request.opset_version,
                                           export_request.simplify);
            onnx_path = export_request.output_path;
        }

        if (onnx_path.empty()) {
            throw std::runtime_error("Provide weights path or ONNX path");
        }

        std::string summary = "Exported ONNX: " + onnx_path.string();

        if (state.build_tensorrt) {
            const BuildEngineRequest build_request = rfdetr_workflows::build_build_engine_request(state, onnx_path);
            append_job_output("[export] building TensorRT engine: " + build_request.output_path.string());
            rfdetr::make_tensorrt_backend(build_request.onnx_path, build_request.device_id, build_request.allow_fp16,
                                          build_request.output_path);
            append_job_output("[export] TensorRT engine ready: " + build_request.output_path.string());
            summary += " | TRT engine: " + build_request.output_path.string();
        }

        JobOutcome outcome;
        outcome.summary = std::move(summary);
        return outcome;
    });
}

bool App::request_annotation_save_now() {
    return annotation_workflow_coordinator_ != nullptr && annotation_workflow_coordinator_->request_save_now();
}

void App::launch_browser_file_dialog(const mmltk::browser::FileDialogRequestIntent& intent) {
    if (active_file_browse_field_ != FileBrowseField::None || shutting_down_) {
        throw std::runtime_error(
            "browser file dialog request blocked while "
            "another file dialog is active");
    }

    const mmltk::browser::BrowserFileDialogBinding binding =
        mmltk::browser::file_dialog_binding_from_id(intent.dialog_id);
    if (binding == mmltk::browser::BrowserFileDialogBinding::Unknown) {
        throw std::runtime_error("unsupported browser file dialog binding: " + intent.dialog_id);
    }

    const mmltk::browser::BrowserFileDialogStateAccess state_access{&train_, &predict_, &annotate_, &validate_,
                                                                    &export_};

    auto file_picker_mode = [mode = intent.mode]() {
        switch (mode) {
            case mmltk::browser::FileDialogMode::OpenFile:
                return FilePickerMode::OpenFile;
            case mmltk::browser::FileDialogMode::OpenFolder:
                return FilePickerMode::OpenFolder;
            case mmltk::browser::FileDialogMode::SaveFile:
                return FilePickerMode::SaveFile;
        }
        return FilePickerMode::OpenFile;
    };

    std::vector<FilePickerFilter> filters;
    filters.reserve(intent.filters.size());
    for (const auto& filter : intent.filters) {
        filters.push_back(FilePickerFilter{filter.name, filter.patterns});
    }

    active_file_browse_field_ = FileBrowseField::BrowserDialog;
    picker_error_.clear();
    const std::string initial_path = !intent.default_path.empty()
                                         ? intent.default_path
                                         : mmltk::browser::browser_file_dialog_initial_path(binding, state_access);
    const std::string dialog_title = intent.title.empty() ? intent.dialog_id : intent.title;
    active_browser_dialog_id_ = intent.dialog_id;
    active_browser_dialog_title_ = dialog_title;
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [dialog_title, initial_path, mode = file_picker_mode(), filters = std::move(filters)]() {
            return pick_path_with_dialog(dialog_title.c_str(), initial_path, mode, filters);
        },
        [this, binding](std::optional<std::string> picked) {
            if (picked.has_value()) {
                mmltk::browser::apply_browser_file_dialog_selection(
                    binding, std::move(*picked),
                    mmltk::browser::BrowserFileDialogStateAccess{&train_, &predict_, &annotate_, &validate_, &export_});
            }
            picker_error_.clear();
            if (active_file_browse_field_ == FileBrowseField::BrowserDialog) {
                active_file_browse_field_ = FileBrowseField::None;
            }
            active_browser_dialog_id_.clear();
            active_browser_dialog_title_.clear();
        },
        [this](const std::string& error) {
            picker_error_ = error;
            if (active_file_browse_field_ == FileBrowseField::BrowserDialog) {
                active_file_browse_field_ = FileBrowseField::None;
            }
            active_browser_dialog_id_.clear();
            active_browser_dialog_title_.clear();
        });
}

void App::queue_browser_viewport_commit(const mmltk::browser::Workflow workflow,
                                        const mmltk::browser::ViewportCommitIntent& intent) {
    browser_workspace_viewport_state_ = BrowserWorkspaceViewportState{
        workflow,
        intent,
    };
    if (workflow == mmltk::browser::Workflow::Predict || workflow == mmltk::browser::Workflow::Live) {
        if (!intent.clip.has_value()) {
            return;
        }
        mmltk::browser::apply_crop_commit(mmltk::browser::CropCommitIntent{true, *intent.clip}, predict_.source);
        if (live_predict_controller_.controller() != nullptr) {
            const auto& clip = *intent.clip;
            publish_runtime_crop_box(live_predict_controller_.controller()->ui_crop_state(), predict_.source,
                                     AnnotationBox{
                                         static_cast<int>(clip.x),
                                         static_cast<int>(clip.y),
                                         static_cast<int>(clip.x + clip.width),
                                         static_cast<int>(clip.y + clip.height),
                                     });
        }
        return;
    }

    cancel_annotation_canvas_interactions();
}

void App::launch_file_picker(FileBrowseField field, const char* dialog_title, std::string* target) {
    if (active_file_browse_field_ != FileBrowseField::None || target == nullptr || shutting_down_) {
        return;
    }
    active_file_browse_field_ = field;
    active_browser_dialog_id_.clear();
    active_browser_dialog_title_ = dialog_title != nullptr ? dialog_title : "";
    picker_error_.clear();
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [dialog = std::string(dialog_title), initial = *target]() {
            return pick_file_with_dialog(dialog.c_str(), initial);
        },
        [this, field, target](std::optional<std::string> picked) {
            if (picked.has_value()) {
                *target = std::move(*picked);
            }
            picker_error_.clear();
            if (active_file_browse_field_ == field) {
                active_file_browse_field_ = FileBrowseField::None;
            }
            active_browser_dialog_title_.clear();
        },
        [this, field](const std::string& error) {
            picker_error_ = error;
            if (active_file_browse_field_ == field) {
                active_file_browse_field_ = FileBrowseField::None;
            }
            active_browser_dialog_title_.clear();
        });
}

bool App::file_picker_busy(FileBrowseField field) const {
    return active_file_browse_field_ == field;
}

void App::append_job_output(std::string_view chunk) {
    std::lock_guard<std::mutex> lock(job_.output_mutex);
    console_output::append_console_output(job_.output_tail, chunk, 65536);
    if (job_.output_tail.empty() || job_.output_tail.back() != '\n') {
        job_.output_tail.push_back('\n');
        console_output::trim_output_tail(job_.output_tail, 65536);
    }
}

std::string App::snapshot_job_output() {
    std::lock_guard<std::mutex> lock(job_.output_mutex);
    return job_.output_tail;
}

void AnnotationWorkflowCoordinator::invalidate_preview() {
    invalidate_annotation_workflow_preview(app_.annotate_workflow_);
}

void AnnotationWorkflowCoordinator::cancel_canvas_interactions() {
    (void)cancel_active_grouped_edit(app_.annotate_document_, app_.annotate_session_);
    app_.annotate_controller_.reset_active_drawing(app_.annotate_session_);
    app_.annotate_document_.cancel_transaction();
}

void AnnotationWorkflowCoordinator::reset_instances() {
    reset_annotation_workflow_runtime(app_.annotate_workflow_);
    app_.annotate_document_.clear();
    app_.annotate_document_.clear_history();
    app_.annotate_resolved_instances_.clear();
    select_object(app_.annotate_session_, app_.annotate_document_, std::nullopt);
    cancel_canvas_interactions();
    invalidate_preview();
}

void AnnotationWorkflowCoordinator::sync_categories() {
    if (app_.annotate_.output_dir.empty()) {
        app_.annotate_categories_loaded_ = false;
        app_.annotate_categories_output_dir_.clear();
        return;
    }
    if (app_.annotate_categories_loaded_ && app_.annotate_categories_output_dir_ == app_.annotate_.output_dir) {
        return;
    }
    try {
        AnnotationCategories loaded_categories = load_annotation_categories(app_.annotate_.output_dir);
        app_.annotate_categories_ = std::move(loaded_categories);
        app_.annotate_categories_loaded_ = true;
        app_.annotate_categories_output_dir_ = app_.annotate_.output_dir;
        (void)AnnotationEditor::repair_object_category_indices(app_.annotate_document_, app_.annotate_categories_);
    } catch (const std::exception& error) {
        app_.annotate_categories_loaded_ = true;
        app_.annotate_categories_output_dir_ = app_.annotate_.output_dir;
        app_.annotate_save_error_ = error.what();
        log_gui_error("annotate categories error", app_.annotate_save_error_);
    }
}

void AnnotationWorkflowCoordinator::prepare_source() {
    const std::string signature = annotation_source_signature(app_.annotate_.source);
    if (signature == app_.annotate_source_signature_) {
        return;
    }

    app_.annotate_source_signature_ = signature;
    ++app_.annotate_prepare_request_id_;
    app_.annotate_inputs_.clear();
    app_.annotate_current_input_index_ = 0;
    app_.annotate_frame_.reset();
    app_.clear_browser_annotate_publications();
    app_.annotate_prepare_running_ = false;
    app_.annotate_frame_load_running_ = false;
    app_.annotate_assist_summary_.clear();
    app_.annotate_assist_error_.clear();
    app_.annotate_save_summary_.clear();
    app_.annotate_save_error_.clear();
    reset_instances();
    reset_live_session_state(false, App::ActiveLiveMode::None);
    if (app_.annotate_.source.kind == SourceKind::VideoStream) {
        return;
    }

    if (app_.annotate_.source.kind == SourceKind::CompiledDataset) {
        app_.annotate_save_error_ = "Annotate only supports single images, image folders, and live video.";
        return;
    }

    const SourceSelectionState source = app_.annotate_.source;
    const std::uint64_t request_id = app_.annotate_prepare_request_id_;
    app_.annotate_prepare_running_ = true;
    mmltk::runtime::submit_background_task(
        app_.background_executor_, app_.ui_callbacks_,
        [source]() {
            PreparedPredictSource prepared = prepare_predict_source(source);
            return std::move(prepared.image_inputs);
        },
        [this, request_id, signature](std::vector<mmltk::rfdetr::PredictImageInput> inputs) {
            if (request_id != app_.annotate_prepare_request_id_ || signature != app_.annotate_source_signature_) {
                return;
            }
            app_.annotate_prepare_running_ = false;
            app_.annotate_inputs_ = std::move(inputs);
            app_.annotate_save_error_.clear();
            if (!app_.annotate_inputs_.empty()) {
                load_current_frame();
            }
        },
        [this, request_id, signature](const std::string& error) {
            if (request_id != app_.annotate_prepare_request_id_ || signature != app_.annotate_source_signature_) {
                return;
            }
            app_.annotate_prepare_running_ = false;
            app_.annotate_save_error_ = error;
            log_gui_error("annotate source error", app_.annotate_save_error_);
        });
}

void AnnotationWorkflowCoordinator::load_current_frame() {
    if (app_.annotate_current_input_index_ >= app_.annotate_inputs_.size()) {
        app_.annotate_frame_.reset();
        app_.clear_browser_annotate_publications();
        app_.annotate_resolved_instances_.clear();
        cancel_canvas_interactions();
        invalidate_preview();
        return;
    }
    ++app_.annotate_frame_request_id_;
    const std::uint64_t request_id = app_.annotate_frame_request_id_;
    const mmltk::rfdetr::PredictImageInput input = app_.annotate_inputs_[app_.annotate_current_input_index_];
    app_.annotate_frame_load_running_ = true;
    mmltk::runtime::submit_background_task(
        app_.background_executor_, app_.ui_callbacks_, [input]() { return mmltk::gui::load_annotation_frame(input); },
        [this, request_id](AnnotationFrame frame) {
            if (request_id != app_.annotate_frame_request_id_) {
                return;
            }
            app_.annotate_frame_load_running_ = false;
            load_frame(std::move(frame));
            app_.annotate_save_error_.clear();
        },
        [this, request_id](const std::string& error) {
            if (request_id != app_.annotate_frame_request_id_) {
                return;
            }
            app_.annotate_frame_load_running_ = false;
            app_.annotate_frame_.reset();
            app_.clear_browser_annotate_publications();
            app_.annotate_resolved_instances_.clear();
            cancel_canvas_interactions();
            invalidate_preview();
            app_.annotate_save_error_ = error;
            log_gui_error("annotate frame load error", app_.annotate_save_error_);
        });
}

void AnnotationWorkflowCoordinator::step_current_frame(const int step) {
    if (step == 0) {
        load_current_frame();
        return;
    }
    const auto next_index = static_cast<std::ptrdiff_t>(app_.annotate_current_input_index_) + step;
    if (next_index < 0) {
        return;
    }
    app_.annotate_current_input_index_ = static_cast<std::size_t>(next_index);
    load_current_frame();
}

void AnnotationWorkflowCoordinator::reset_live_session_state(const bool clear_live_preview_error,
                                                             const App::ActiveLiveMode active_mode) {
    stop_annotation_workflow_hold_save(app_.annotate_workflow_);
    cancel_canvas_interactions();
    clear_annotation_workflow_live_overlay_state(app_.annotate_workflow_);
    app_.release_browser_live_frame_slots();
    if (app_.live_controller_) {
        app_.live_controller_->stop();
        app_.live_controller_.reset();
    }
    app_.live_session_status_.reset();
    app_.annotate_frame_.reset();
    app_.annotate_resolved_instances_.clear();
    reset_annotation_workflow_preview(app_.annotate_workflow_, false);
    if (clear_live_preview_error) {
        app_.live_preview_error_.clear();
    }
    app_.active_live_mode_ = active_mode;
}

void AnnotationWorkflowCoordinator::handle_setup_browse_request(const AnnotationSetupBrowseRequest browse_request) {
    if (browse_request == AnnotationSetupBrowseRequest::SingleImage) {
        app_.launch_file_picker(App::FileBrowseField::AnnotateSingleImage, "Select Image",
                                &app_.annotate_.source.single_image_path);
        return;
    }

    ModelInputBrowseRequest model_request = ModelInputBrowseRequest::None;
    switch (browse_request) {
        case AnnotationSetupBrowseRequest::Weights:
            model_request = ModelInputBrowseRequest::Weights;
            break;
        case AnnotationSetupBrowseRequest::Onnx:
            model_request = ModelInputBrowseRequest::Onnx;
            break;
        case AnnotationSetupBrowseRequest::TensorRt:
            model_request = ModelInputBrowseRequest::TensorRt;
            break;
        default:
            break;
    }
    switch (model_request) {
        case ModelInputBrowseRequest::Weights:
            app_.annotate_.model_input = ModelInputMode::Weights;
            app_.launch_file_picker(App::FileBrowseField::AnnotateWeights, "Select Weights",
                                    &app_.annotate_.weights_path);
            break;
        case ModelInputBrowseRequest::Onnx:
            app_.annotate_.model_input = ModelInputMode::Onnx;
            app_.launch_file_picker(App::FileBrowseField::AnnotateOnnx, "Select ONNX Model", &app_.annotate_.onnx_path);
            break;
        case ModelInputBrowseRequest::TensorRt:
            app_.annotate_.model_input = ModelInputMode::TensorRt;
            app_.launch_file_picker(App::FileBrowseField::AnnotateTensorRt, "Select TensorRT Engine",
                                    &app_.annotate_.tensorrt_path);
            break;
        case ModelInputBrowseRequest::None:
            break;
    }
};

void AnnotationWorkflowCoordinator::load_frame(AnnotationFrame frame) {
    const bool reset_canvas_view =
        annotation_workflow_should_reset_canvas_view(annotation_frame_ptr(app_.annotate_frame_), frame);
    if (app_.annotate_.source.kind != SourceKind::VideoStream) {
        reset_instances();
    } else {
        app_.annotate_resolved_instances_.clear();
    }
    if (reset_canvas_view) {
        app_.annotate_canvas_scale_ = 0.0f;
        app_.annotate_canvas_pan_x_ = 0.0f;
        app_.annotate_canvas_pan_y_ = 0.0f;
        app_.annotate_canvas_auto_fit_ = true;
    }
    app_.clear_browser_annotate_publications();
    app_.annotate_frame_ = std::move(frame);
    const AnnotationFrame* annotation_frame = optional_value_ptr(app_.annotate_frame_);
    if (app_.annotate_.source.kind != SourceKind::VideoStream && annotation_frame != nullptr) {
        app_.publish_browser_annotation_workspace_frame(*annotation_frame);
    }
    if (app_.annotate_.source.kind != SourceKind::VideoStream && annotation_frame != nullptr &&
        !app_.annotate_.output_dir.empty()) {
        sync_categories();
        try {
            const std::optional<std::vector<AnnotationObject>> loaded_objects = load_saved_annotation_scene_for_frame(
                app_.annotate_.output_dir, *annotation_frame, &app_.annotate_categories_);
            if (loaded_objects.has_value()) {
                app_.annotate_document_.set_objects(*loaded_objects, true);
                if (!app_.annotate_document_.empty()) {
                    select_object(app_.annotate_session_, app_.annotate_document_, 0U);
                }
            }
        } catch (const std::exception& error) {
            app_.annotate_save_error_ = error.what();
            log_gui_error("annotate scene load error", app_.annotate_save_error_);
        }
    }
    invalidate_preview();
}

void AnnotationWorkflowCoordinator::submit_preview() {
    const AnnotationFrame* frame_ptr = annotation_frame_ptr(app_.annotate_frame_);
    if (frame_ptr == nullptr || annotation_workflow_preview_running(app_.annotate_workflow_)) {
        return;
    }
    const bool live_mode = live_running();
    if (live_mode) {
        AnnotationWorkflowLivePreviewRefreshResult refreshed = refresh_annotation_workflow_live_preview(
            app_.annotate_workflow_, app_.annotate_document_, app_.annotate_session_, *frame_ptr,
            app_.annotate_categories_, !frame_ptr->pixels_bgr.empty(),
            [this](mmltk::live::ManualOverlayDocumentSnapshot snapshot) {
                app_.live_controller_->manual_overlay_document().publish_snapshot(std::move(snapshot));
            });
        if (refreshed.ok) {
            if (!frame_ptr->pixels_bgr.empty()) {
                app_.annotate_resolved_instances_ = std::move(refreshed.resolved_objects);
            }
            app_.live_preview_error_.clear();
        } else {
            app_.annotate_save_error_ = std::move(refreshed.error_message);
            app_.live_preview_error_ = app_.annotate_save_error_;
        }
        return;
    }

    const AnnotationFrame frame_snapshot = *frame_ptr;
    const AnnotationCategories categories_snapshot = app_.annotate_categories_;
    const AnnotationDocumentSnapshot document_snapshot = app_.annotate_document_.snapshot();
    const std::shared_ptr<const AnnotationProjectedScene> projected_scene_snapshot =
        resolve_annotation_projected_scene_for_document_generation(app_.annotate_workflow_, app_.annotate_document_,
                                                                   app_.annotate_session_, app_.annotate_frame_,
                                                                   document_snapshot.generation);
    const std::uint64_t generation = begin_annotation_workflow_preview(app_.annotate_workflow_);
    mmltk::runtime::submit_background_task(
        app_.background_executor_, app_.ui_callbacks_,
        [frame_snapshot, categories_snapshot, document_snapshot, projected_scene_snapshot]() {
            return prepare_annotation_workflow_preview(frame_snapshot, categories_snapshot, document_snapshot,
                                                       projected_scene_snapshot.get());
        },
        [this, generation](const AnnotationWorkflowPreparedPreview& prepared) mutable {
            end_annotation_workflow_preview(app_.annotate_workflow_);
            const AnnotationFrame* annotation_frame = optional_value_ptr(app_.annotate_frame_);
            if (annotation_workflow_preview_generation_matches(app_.annotate_workflow_, generation) &&
                annotation_frame != nullptr && annotation_frame->frame_id == prepared.frame_id) {
                app_.annotate_resolved_instances_ = prepared.preview.resolved_objects;
                const bool preview_ready =
                    app_.browser_publication_state_ != nullptr && !annotation_frame->pixels_bgr.empty();
                if (preview_ready) {
                    app_.live_preview_error_.clear();
                    note_annotation_workflow_preview_ready(app_.annotate_workflow_, *annotation_frame);
                }
            }
            if (annotation_workflow_preview_pending(app_.annotate_workflow_,
                                                    annotation_frame_ptr(app_.annotate_frame_))) {
                submit_preview();
            }
        },
        [this](const std::string& error) {
            app_.annotate_save_error_ = error;
            app_.annotate_resolved_instances_.clear();
            app_.live_preview_error_ = error;
            note_annotation_workflow_preview_error(app_.annotate_workflow_);
            constexpr int kMaxPreviewRetries = 3;
            if (annotation_workflow_preview_can_retry(app_.annotate_workflow_, kMaxPreviewRetries) &&
                annotation_workflow_preview_pending(app_.annotate_workflow_,
                                                    annotation_frame_ptr(app_.annotate_frame_))) {
                submit_preview();
            }
        });
}

bool AnnotationWorkflowCoordinator::live_running() const {
    return app_.active_live_mode_ == App::ActiveLiveMode::Annotate && app_.live_controller_ != nullptr;
}

bool AnnotationWorkflowCoordinator::ensure_live_annotation_frame_pixels(std::string* error_message) const {
    return ensure_annotation_workflow_live_annotation_frame_pixels(app_.annotate_.source, app_.annotate_frame_,
                                                                   live_running(), app_.live_controller_.get(),
                                                                   app_.annotate_.full_frame, error_message);
}

AnnotationWorkflowSaveRequest AnnotationWorkflowCoordinator::make_save_request() const {
    return make_annotation_workflow_save_request(
        &app_.annotate_workflow_, app_.annotate_, live_running(), app_.annotate_save_running_,
        annotation_frame_ptr(app_.annotate_frame_), !app_.annotate_resolved_instances_.empty(),
        app_.annotate_current_input_index_, app_.annotate_inputs_.size());
}

void AnnotationWorkflowCoordinator::log_annotation_resolve_error(const char* error_context,
                                                                 const std::string& error) const {
    log_gui_error(error_context != nullptr ? error_context : "annotate resolve error", error);
}

std::optional<AnnotationSaveSnapshot> AnnotationWorkflowCoordinator::make_current_save_snapshot(
    const bool refresh_live_frame, const char* resolve_error_context) {
    return make_annotation_workflow_current_save_snapshot(
        AnnotationWorkflowCurrentSaveSnapshotRequest{
            &app_.annotate_workflow_,
            &app_.annotate_document_,
            &app_.annotate_session_,
            &app_.annotate_frame_,
            &app_.annotate_categories_,
            &app_.annotate_resolved_instances_,
            refresh_live_frame,
            true,
            &app_.annotate_save_error_,
            resolve_error_context,
        },
        [this](std::string* error_message) { return ensure_live_annotation_frame_pixels(error_message); },
        [this](const char* error_context, const std::string& error) {
            log_annotation_resolve_error(error_context, error);
        });
}

bool AnnotationWorkflowCoordinator::launch_save(AnnotationSaveSnapshot save_snapshot, const bool live_mode) {
    sync_categories();
    return launch_annotation_workflow_save(
        app_.annotate_workflow_, app_.annotate_, app_.annotate_categories_, std::move(save_snapshot), live_mode,
        AnnotationWorkflowSaveLaunchState{&app_.annotate_save_running_, &app_.annotate_save_summary_,
                                          &app_.annotate_save_error_, &app_.annotate_categories_loaded_,
                                          &app_.annotate_categories_output_dir_},
        app_.background_executor_, app_.ui_callbacks_,
        [](const std::string& error) { log_gui_error("annotate save error", error); });
}

void AnnotationWorkflowCoordinator::poll() {
    if (live_running()) {
        std::optional<AnnotationFrame> synced_frame = app_.make_live_annotation_frame_from_preview();
        if (synced_frame.has_value()) {
            load_frame(std::move(*synced_frame));
        }
        if (app_.live_controller_) {
            const std::string controller_error = app_.live_controller_->last_error();
            if (!controller_error.empty()) {
                app_.annotate_save_error_ = controller_error;
            }
        }
    }

    const AnnotationWorkflowPollPlan workflow_poll = plan_annotation_workflow_poll(make_save_request());

    drive_annotation_save_pipeline(
        workflow_poll.pipeline_plan,
        [this]() -> std::optional<AnnotationSaveSnapshot> {
            return make_current_save_snapshot(true, "annotate hold-save resolve error");
        },
        [this](AnnotationSaveSnapshot save_snapshot) {
            queue_annotation_workflow_save(app_.annotate_workflow_, std::move(save_snapshot.frame),
                                           std::move(save_snapshot.objects), std::move(save_snapshot.projected_scene));
        },
        [this](AnnotationSaveSnapshot save_snapshot) { (void)launch_save(std::move(save_snapshot), true); },
        [this]() { clear_annotation_workflow_save_queue(app_.annotate_workflow_); },
        [this]() { return take_annotation_workflow_queued_save(app_.annotate_workflow_); },
        [this](AnnotationQueuedSave queued_save) {
            (void)launch_save(
                AnnotationSaveSnapshot{
                    std::move(queued_save.frame),
                    std::move(queued_save.objects),
                    std::move(queued_save.projected_scene),
                },
                true);
        },
        [this]() {
            mark_annotation_hold_save_overflow(app_.annotate_workflow_);
            app_.annotate_save_error_ =
                "Hold Save overflowed the single-frame writer queue. Capture "
                "paused to avoid dropping frames silently.";
            log_gui_error("annotate hold-save overflow", app_.annotate_save_error_);
        });
}

void AnnotationWorkflowCoordinator::shutdown() {
    reset_live_session_state(true, App::ActiveLiveMode::None);
}

void AnnotationWorkflowCoordinator::start_live_session() {
    reset_live_session_state(true, App::ActiveLiveMode::None);

    app_.annotate_save_error_.clear();
    try {
        if (app_.annotate_.source.kind != SourceKind::VideoStream) {
            throw std::runtime_error("annotation live capture requires a video source");
        }

        mmltk::live::LiveSessionConfig session_config;
        session_config.capture.device_path =
            "/dev/video" + std::to_string(std::max(0, app_.annotate_.source.device_index));
        session_config.capture.cuda_device_index = app_.annotate_.device_id;
        session_config.capture.width = static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_width));
        session_config.capture.height = static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_height));
        session_config.capture.fps = static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_fps));
        session_config.capture.v4l2_buffer_count =
            static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.v4l2_buffer_count));
        session_config.capture.preview_buffer_count = 1U;
        const mmltk::live::LiveCaptureRegion initial_region = full_capture_region_for_source(app_.annotate_.source);
        session_config.capture.initial_region = frameshow::CaptureRegion{
            initial_region.x,
            initial_region.y,
            initial_region.width,
            initial_region.height,
        };
        session_config.detect_slot_count = 2U;
        session_config.output_slot_count = 4U;
        session_config.cuda_device_index = app_.annotate_.device_id;

        auto controller = std::make_unique<mmltk::live::LiveSessionController>(std::move(session_config));
        controller->set_workspace_ready_callback(app_.live_workspace_ready_callback());
        controller->start();
        controller->manual_overlay_document().clear(
            static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_width)),
            static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_height)));
        app_.live_session_status_ = std::make_unique<mmltk::live::LiveSessionStatus>(controller->snapshot_status());
        app_.live_controller_ = std::move(controller);
        app_.active_live_mode_ = App::ActiveLiveMode::Annotate;
    } catch (const std::exception& error) {
        app_.active_live_mode_ = App::ActiveLiveMode::None;
        app_.annotate_save_error_ = error.what();
    }
}

void AnnotationWorkflowCoordinator::stop_live_session() {
    reset_live_session_state(true, App::ActiveLiveMode::None);
}

void App::invalidate_annotation_preview() {
    annotation_workflow_coordinator_->invalidate_preview();
}

void App::cancel_annotation_canvas_interactions() {
    annotation_workflow_coordinator_->cancel_canvas_interactions();
}

void App::reset_annotation_instances() {
    annotation_workflow_coordinator_->reset_instances();
}

bool App::annotation_live_running() const {
    return annotation_workflow_coordinator_->live_running();
}

void App::poll_annotate_work() {
    annotation_workflow_coordinator_->poll();
}

std::vector<RemoteGpuFamily> App::selected_remote_gpu_families() const {
    return ::mmltk::gui::selected_remote_gpu_families(train_.remote_family_enabled);
}

bool App::live_predict_running() const {
    return live_predict_controller_.running();
}

bool App::live_predict_active() const {
    return live_predict_controller_.active();
}

std::optional<AnnotationFrame> App::make_live_annotation_frame_from_preview() const {
    if (!annotation_live_running() || live_controller_ == nullptr) {
        return std::nullopt;
    }

    mmltk::live::WorkspaceOutputBundle output_bundle{};
    if (!live_controller_->try_acquire_latest_workspace(&output_bundle)) {
        return std::nullopt;
    }
    const auto release_workspace_slot = [this, slot_index = output_bundle.slot_index]() {
        live_controller_->release_workspace(slot_index);
    };

    const mmltk::live::LiveCaptureRegion region =
        preview_region_for_source(output_bundle.region, annotate_.source, annotate_.full_frame);
    const mmltk::live::LiveCaptureRegion displayed_region{
        region.x,
        region.y,
        region.width == 0U ? output_bundle.dims.width : region.width,
        region.height == 0U ? output_bundle.dims.height : region.height,
    };
    const AnnotationFrame* current_annotation_frame = optional_value_ptr(annotate_frame_);
    if (current_annotation_frame != nullptr && current_annotation_frame->live_frame_id == output_bundle.frame_id &&
        current_annotation_frame->view_x == displayed_region.x &&
        current_annotation_frame->view_y == displayed_region.y &&
        current_annotation_frame->width == displayed_region.width &&
        current_annotation_frame->height == displayed_region.height) {
        release_workspace_slot();
        return std::nullopt;
    }

    AnnotationFrame frame;
    frame.source_name = "live";
    frame.source_path = "/dev/video" + std::to_string(std::max(0, annotate_.source.device_index));
    frame.frame_id = output_bundle.frame_id.sequence;
    frame.live_frame_id = output_bundle.frame_id;
    frame.width = displayed_region.width;
    frame.height = displayed_region.height;
    frame.view_x = displayed_region.x;
    frame.view_y = displayed_region.y;
    frame.capture_width = static_cast<std::uint32_t>(std::max(1, annotate_.source.capture_width));
    frame.capture_height = static_cast<std::uint32_t>(std::max(1, annotate_.source.capture_height));
    release_workspace_slot();
    return frame;
}

bool App::has_static_preview() const {
    return !live_predict_running() && browser_publication_state_ != nullptr &&
           browser_publication_state_->predict_preview_source.has_value() && !live_static_preview_source_name_.empty();
}

void App::clear_static_preview() {
    if (live_predict_running()) {
        return;
    }
    clear_browser_predict_publication();
    if (browser_publication_state_ != nullptr) {
        browser_publication_state_->predict_preview_source.reset();
    }
    live_static_preview_source_name_.clear();
    live_predict_controller_.clear_errors();
    live_preview_error_.clear();
}

void App::begin_live_predict_preview_stream(const int device_id) {
    if (browser_publication_state_ != nullptr) {
        browser_publication_state_->live_cuda_device_index = device_id;
    }
    active_live_mode_ = ActiveLiveMode::Predict;
}

void App::end_live_predict_preview_stream() {
    if (browser_publication_state_ != nullptr) {
        browser_publication_state_->live_cuda_device_index.reset();
    }
    active_live_mode_ = ActiveLiveMode::None;
}

void App::reset_live_predict_preview_state(const bool clear_frame) {
    if (clear_frame) {
        clear_browser_predict_publication();
    }
    if (browser_publication_state_ != nullptr && active_live_mode_ == ActiveLiveMode::None) {
        browser_publication_state_->live_cuda_device_index.reset();
    }
    live_static_preview_source_name_.clear();
    live_preview_error_.clear();
    live_crop_overlay_mode_ = false;
    live_crop_drag_session_ = {};
}

mmltk::live::LiveSessionController* App::active_browser_live_controller() noexcept {
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (active_live_mode_ == ActiveLiveMode::Annotate) {
        return live_controller_.get();
    }
    if (mmltk::live::LiveSessionController* controller = live_predict_controller_.controller(); controller != nullptr) {
        return controller;
    }
    return live_controller_.get();
#else
    return nullptr;
#endif
}

void App::refresh_browser_live_frame_publications(mmltk::live::LiveSessionController* browser_live_controller,
                                                  std::string* browser_preview_error) {
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (browser_publication_state_ == nullptr) {
        return;
    }

    BrowserPublicationState& published = *browser_publication_state_;
    if (published.live_controller != browser_live_controller) {
        release_browser_live_frame_slots();
        published.live_controller = browser_live_controller;
    }
    if (browser_live_controller == nullptr) {
        return;
    }

    const int live_cuda_device_index = published.live_cuda_device_index.value_or(predict_.device_id);
    mmltk::live::ensure_cuda_ok(cudaSetDevice(live_cuda_device_index),
                                "cudaSetDevice for browser live frame publication");
    mmltk::live::WorkspaceOutputBundle workspace_bundle{};
    if (browser_live_controller->try_acquire_latest_workspace(&workspace_bundle)) {
        const std::uint32_t workspace_slot_index = workspace_bundle.slot_index;
        try {
            if (live_frame_not_newer_than_current_publication(published.workspace_surface_bridge,
                                                              workspace_bundle.frame_id)) {
                release_live_workspace_slot_silently(browser_live_controller, workspace_slot_index);
                return;
            }
            const mmltk::browser::FrameSlotDescriptor descriptor =
                mmltk::browser::frame_slot_from_workspace_output_bundle(
                    "workspace", mmltk::browser::FrameTransportKind::CudaDeviceBuffer,
                    mmltk::browser::FramePixelFormat::Rgba8, workspace_bundle);
            publish_workspace_surface(
                published.workspace_surface_bridge, BrowserWorkspaceSurfaceOwner::Live,
                make_retained_browser_imported_frame_source(descriptor, workspace_bundle, live_cuda_device_index),
                [browser_live_controller, workspace_slot_index]() noexcept {
                    release_live_workspace_slot_silently(browser_live_controller, workspace_slot_index);
                },
                workspace_bundle.frame_id);
        } catch (...) {
            release_live_workspace_slot_silently(browser_live_controller, workspace_slot_index);
            throw;
        }
    }
    if (browser_preview_error != nullptr &&
        !(published.workspace_surface_bridge.owner() == BrowserWorkspaceSurfaceOwner::Live &&
          published.workspace_surface_bridge.preview().has_frame)) {
        *browser_preview_error = browser_live_controller->last_error();
    }
#else
    (void)browser_live_controller;
    (void)browser_preview_error;
#endif
}

void App::release_browser_live_frame_slots() {
    if (browser_publication_state_ == nullptr) {
        return;
    }

    BrowserPublicationState& published = *browser_publication_state_;
    if (published.live_cuda_device_index.has_value()) {
        mmltk::live::ensure_cuda_ok(cudaSetDevice(*published.live_cuda_device_index),
                                    "cudaSetDevice for browser live frame release");
    }
    published.workspace_surface_bridge.clear_if_owner(BrowserWorkspaceSurfaceOwner::Live);
    published.live_controller = nullptr;
    published.live_cuda_device_index.reset();
}

void App::clear_browser_predict_publication() {
    if (browser_publication_state_ != nullptr) {
        browser_publication_state_->workspace_surface_bridge.clear_if_owner(
            BrowserWorkspaceSurfaceOwner::PredictStatic);
    }
}

void App::clear_browser_annotate_publications() {
    if (browser_publication_state_ != nullptr) {
        browser_publication_state_->workspace_surface_bridge.clear_if_owner(BrowserWorkspaceSurfaceOwner::Annotate);
    }
}

void App::refresh_browser_retained_frame_publications(const bool static_preview_visible) {
    if (browser_publication_state_ == nullptr) {
        return;
    }

    BrowserPublicationState& published = *browser_publication_state_;
    if (current_view_ != View::Annotate || annotate_.source.kind == SourceKind::VideoStream) {
        clear_browser_annotate_publications();
    }
    if (!static_preview_visible) {
        clear_browser_predict_publication();
    }

    if (current_view_ == View::Annotate && annotate_.source.kind != SourceKind::VideoStream) {
        const AnnotationFrame* annotation_frame = optional_value_ptr(annotate_frame_);
        if (published.workspace_surface_bridge.owner() != BrowserWorkspaceSurfaceOwner::Annotate &&
            annotation_frame != nullptr && !annotation_frame->pixels_bgr.empty()) {
            publish_browser_annotation_workspace_frame(*annotation_frame);
        }
        return;
    }

    if (static_preview_visible &&
        published.workspace_surface_bridge.owner() != BrowserWorkspaceSurfaceOwner::PredictStatic &&
        published.predict_preview_source.has_value() && published.predict_preview_source->ready()) {
        const BrowserPublicationState::PredictStaticPreviewSource& source = *published.predict_preview_source;
        const std::uint64_t row_stride_bytes = static_cast<std::uint64_t>(source.width) * 3U;
        const mmltk::live::LiveFrameId frame_id =
            next_browser_frame_id(published.predict_session_nonce, published.predict_sequence);
        mmltk::live::ensure_cuda_ok(cudaSetDevice(predict_.device_id),
                                    "cudaSetDevice for browser retained preview publication");
        publish_retained_imported_browser_frame(
            published, published.retained_publication_slots, std::span<const std::uint8_t>(*source.pixels_bgr),
            predict_.device_id, BrowserWorkspaceSurfaceOwner::PredictStatic, "workspace", 0U, frame_id,
            mmltk::browser::CaptureRegion{0U, 0U, source.width, source.height}, source.width, source.height,
            row_stride_bytes, next_browser_frame_session_nonce(), false);
    }
}

void App::publish_browser_predict_static_preview(StillImagePreview preview) {
    if (browser_publication_state_ == nullptr || preview.width == 0U || preview.height == 0U ||
        preview.pixels_bgr.empty()) {
        if (browser_publication_state_ != nullptr) {
            clear_browser_predict_publication();
            browser_publication_state_->predict_preview_source.reset();
        }
        return;
    }

    BrowserPublicationState& published = *browser_publication_state_;
    clear_browser_predict_publication();
    auto pixels_bgr = std::make_shared<const std::vector<std::uint8_t>>(std::move(preview.pixels_bgr));
    const std::uint64_t row_stride_bytes = static_cast<std::uint64_t>(preview.width) * 3U;
    const mmltk::live::LiveFrameId frame_id =
        next_browser_frame_id(published.predict_session_nonce, published.predict_sequence);
    mmltk::live::ensure_cuda_ok(cudaSetDevice(predict_.device_id),
                                "cudaSetDevice for browser static preview publication");
    publish_retained_imported_browser_frame(
        published, published.retained_publication_slots, std::span<const std::uint8_t>(*pixels_bgr), predict_.device_id,
        BrowserWorkspaceSurfaceOwner::PredictStatic, "workspace", 0U, frame_id,
        mmltk::browser::CaptureRegion{0U, 0U, preview.width, preview.height}, preview.width, preview.height,
        row_stride_bytes, next_browser_frame_session_nonce(), false);
    published.predict_preview_source = BrowserPublicationState::PredictStaticPreviewSource{
        preview.source_name,
        preview.width,
        preview.height,
        pixels_bgr,
    };
}

void App::publish_browser_annotation_workspace_frame(const AnnotationFrame& frame) {
    if (browser_publication_state_ == nullptr || frame.width == 0U || frame.height == 0U || frame.pixels_bgr.empty()) {
        if (browser_publication_state_ != nullptr) {
            clear_browser_annotate_publications();
        }
        return;
    }

    BrowserPublicationState& published = *browser_publication_state_;
    clear_browser_annotate_publications();
    const std::uint64_t row_stride_bytes = static_cast<std::uint64_t>(frame.width) * 3U;
    const mmltk::live::LiveFrameId frame_id =
        next_browser_frame_id(published.annotate_session_nonce, published.annotate_workspace_sequence);
    mmltk::live::ensure_cuda_ok(cudaSetDevice(annotate_.device_id),
                                "cudaSetDevice for browser annotate workspace publication");
    publish_retained_imported_browser_frame(
        published, published.retained_publication_slots, std::span<const std::uint8_t>(frame.pixels_bgr),
        annotate_.device_id, BrowserWorkspaceSurfaceOwner::Annotate, "workspace", 0U, frame_id,
        browser_capture_region_from_annotation_frame(frame), frame.width, frame.height, row_stride_bytes,
        next_browser_frame_session_nonce(), annotation_frame_uses_capture_subregion(frame));
}

void App::apply_static_preview(StillImagePreview preview) {
    if (live_predict_running()) {
        throw std::runtime_error("cannot apply a static preview while live predict is running");
    }

    const std::string source_name = preview.source_name;
    publish_browser_predict_static_preview(std::move(preview));

    const bool preview_published =
        browser_publication_state_ != nullptr && browser_publication_state_->predict_preview_source.has_value();
    live_static_preview_source_name_ = preview_published ? source_name : std::string{};
    live_predict_controller_.clear_errors();
    live_preview_error_.clear();
}

void App::launch_live_predict_session() {
    if (live_predict_running() || shutting_down_) {
        return;
    }

    clear_static_preview();
    live_predict_controller_.clear_errors();
    live_preview_error_.clear();
    live_predict_controller_.launch(
        background_executor_, ui_callbacks_, predict_, selected_preset_name_, live_workspace_ready_callback(),
        [this](const int device_id) {
            begin_live_predict_preview_stream(device_id);
            refresh_live_workspace_ready_callbacks();
            current_view_ = View::Live;
        },
        [this](const std::string& error) { log_gui_error("live start error", error); });
}

void App::stop_live_predict_session() {
    release_browser_live_frame_slots();
    if (live_predict_controller_.starting()) {
        live_predict_controller_.stop(background_executor_, ui_callbacks_, &predict_.source);
    }
    if (live_predict_controller_.stopping()) {
        return;
    }
    if (active_live_mode_ != ActiveLiveMode::Predict || live_predict_controller_.controller() == nullptr) {
        reset_live_predict_preview_state(!live_predict_running() && active_live_mode_ == ActiveLiveMode::None);
        live_predict_controller_.clear_errors();
        return;
    }

    reset_live_predict_preview_state(false);
    live_predict_controller_.stop(
        background_executor_, ui_callbacks_, &predict_.source, [this]() { end_live_predict_preview_stream(); },
        [this]() { reset_live_predict_preview_state(true); },
        [this](const std::string& error) {
            reset_live_predict_preview_state(true);
            log_gui_error("live stop error", error);
        });
}

void App::apply_selected_preset_defaults() {
    const auto& preset = resolve_selected_preset(selected_preset_name_);
    train_.eval_max_dets = preset.num_select;
    apply_train_recipe(train_, current_train_recipe(selected_preset_name_, train_.optimizer), train_.recipe_overrides);
    validate_.resolution = preset.resolution;
    validate_.eval_max_dets = preset.num_select;
    predict_.max_dets_per_image = preset.num_select;
    annotate_.max_dets_per_image = preset.num_select;
}

bool AnnotationWorkflowCoordinator::dispatch_save_now(const AnnotationSaveTabPlan& save_tab_plan) {
    sync_categories();
    return dispatch_annotation_workflow_save_now(
        save_tab_plan,
        [this](const bool refresh_live_frame) -> std::optional<AnnotationSaveSnapshot> {
            return make_current_save_snapshot(refresh_live_frame, "annotate live save resolve error");
        },
        [this](AnnotationSaveSnapshot save_snapshot, const bool live_mode) {
            return launch_save(std::move(save_snapshot), live_mode);
        },
        [this]() { step_current_frame(1); });
}

bool AnnotationWorkflowCoordinator::request_save_now() {
    return dispatch_save_now(plan_annotation_workflow_poll(make_save_request()).save_tab_plan);
}

#undef current_view_
#undef selected_preset_name_
#undef ui_settings_
#undef train_
#undef validate_
#undef predict_
#undef annotate_
#undef export_
#undef job_
#undef annotate_inputs_
#undef annotate_source_signature_
#undef annotate_current_input_index_
#undef annotate_frame_
#undef annotate_document_
#undef annotate_resolved_instances_
#undef annotate_session_
#undef annotate_controller_
#undef annotate_categories_
#undef annotate_categories_loaded_
#undef annotate_categories_output_dir_
#undef annotate_pending_class_name_
#undef annotate_brush_radius_
#undef annotate_cleanup_radius_
#undef annotate_workflow_
#undef annotate_canvas_scale_
#undef annotate_canvas_pan_x_
#undef annotate_canvas_pan_y_
#undef annotate_canvas_auto_fit_
#undef annotate_canvas_panning_
#undef annotate_prepare_running_
#undef annotate_frame_load_running_
#undef annotate_prepare_request_id_
#undef annotate_frame_request_id_
#undef annotate_assist_running_
#undef annotate_assist_summary_
#undef annotate_assist_error_
#undef annotate_save_running_
#undef annotate_save_summary_
#undef annotate_save_error_
#undef live_controller_
#undef live_session_status_
#undef active_live_mode_
#undef ui_callbacks_
#undef background_executor_
#undef live_predict_controller_
#undef vast_query_controller_
#undef remote_train_controller_
#undef local_train_controller_
#undef live_static_preview_source_name_
#undef live_preview_error_
#undef live_preview_fit_to_capture_
#undef live_crop_overlay_mode_
#undef shutting_down_
#undef live_crop_drag_session_
#undef active_file_browse_field_
#undef active_browser_dialog_id_
#undef active_browser_dialog_title_
#undef picker_error_
#undef vast_api_key_
#undef model_registry_
#undef settings_persistence_
#undef annotation_workflow_coordinator_
#undef browser_publication_state_
#undef browser_workspace_viewport_state_

}  // namespace mmltk::gui
