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
#include "browser_artifact_controller.h"
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
#include "live/live_worker_logging.h"
#include "live_predict_controller.h"
#include "live_session_utils.h"
#include "local_train_controller.h"
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
#include "mmltk/rfdetr/preset_catalog.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/validate.h"
#include "mmltk/live/live_pipeline_trace.h"
#include "rfdetr/backends.h"
#include "rfdetr/inference/backend_factory.h"

#include <c10/cuda/CUDAGuard.h>
#include <torch/torch.h>

#if MMLTK_RFDETR_LIVE_CAPTURE
#include "mmltk/live/live_session_controller.h"
#include "mmltk/live/static_workspace_compositor.h"
#include "mmltk/live/workspace_surface_pool.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

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
constexpr auto kAnnotateLiveStartupDeadline = std::chrono::milliseconds(2500);

enum class AnnotateLiveStartupState : std::uint8_t {
    Idle = 0,
    Starting = 1,
    Running = 2,
    Failed = 3,
};

[[nodiscard]] const char* annotate_live_startup_state_name(const AnnotateLiveStartupState state) noexcept {
    switch (state) {
        case AnnotateLiveStartupState::Starting:
            return "starting";
        case AnnotateLiveStartupState::Running:
            return "running";
        case AnnotateLiveStartupState::Failed:
            return "failed";
        case AnnotateLiveStartupState::Idle:
            break;
    }
    return "idle";
}

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

[[nodiscard]] const char* model_input_log_name(const ModelInputMode mode) noexcept {
    switch (mode) {
        case ModelInputMode::Weights:
            return "Weights";
        case ModelInputMode::Onnx:
            return "ONNX";
        case ModelInputMode::TensorRt:
            return "TensorRT";
        case ModelInputMode::None:
            return "None";
    }
    return "None";
}

[[nodiscard]] nlohmann::json annotation_box_to_json(const AnnotationBox& box) {
    return {
        {"x1", box.x1},
        {"y1", box.y1},
        {"x2", box.x2},
        {"y2", box.y2},
    };
}

[[nodiscard]] bool preview_rect_drag_sessions_equal(const PreviewRectDragSession& lhs,
                                                    const PreviewRectDragSession& rhs) noexcept {
    const auto boxes_equal = [](const AnnotationBox& left, const AnnotationBox& right) {
        return left.x1 == right.x1 && left.y1 == right.y1 && left.x2 == right.x2 && left.y2 == right.y2;
    };
    return lhs.active == rhs.active && lhs.drag.kind == rhs.drag.kind &&
           lhs.drag.start_mouse_x == rhs.drag.start_mouse_x && lhs.drag.start_mouse_y == rhs.drag.start_mouse_y &&
           boxes_equal(lhs.drag.start_box, rhs.drag.start_box) && boxes_equal(lhs.draft_box, rhs.draft_box) &&
           lhs.commit_min_extent == rhs.commit_min_extent;
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

bool annotation_source_selected(const SourceSelectionState& source) noexcept {
    switch (source.kind) {
        case SourceKind::CompiledDataset:
            return !source.compiled_path.empty();
        case SourceKind::SingleImage:
            return !source.single_image_path.empty();
        case SourceKind::ImageFolder:
            return !source.image_directory.empty();
        case SourceKind::VideoStream:
            return true;
    }
    return false;
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

const ModelPresetConfig& resolve_model_preset(std::string& preset_name) {
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

WorkflowModelSelectionState& workflow_model_selection(const View view, TrainViewState& train,
                                                      ValidateViewState& validate, PredictViewState& predict,
                                                      AnnotateViewState& annotate, ExportViewState& export_state) {
    switch (view) {
        case View::Train:
            return train;
        case View::Validate:
            return validate;
        case View::Predict:
        case View::Live:
            return predict;
        case View::Annotate:
            return annotate;
        case View::Export:
            return export_state;
    }
    return train;
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
        {"val_loss", progress.val_loss.has_value() ? nlohmann::json(*progress.val_loss) : nlohmann::json(nullptr)},
        {"val_bbox_ap", progress.val_bbox_ap},
        {"val_mask_ap",
         progress.val_mask_ap.has_value() ? nlohmann::json(*progress.val_mask_ap) : nlohmann::json(nullptr)},
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

[[nodiscard]] bool split_paths_match_directory_defaults(const TrainViewState& train) {
    if (train.compiled_dataset_dir.empty()) {
        return train.train_compiled_path.empty() && train.val_compiled_path.empty() && train.test_compiled_path.empty();
    }
    const std::filesystem::path directory(train.compiled_dataset_dir);
    const auto matches = [](const std::string& actual, const std::filesystem::path& expected) {
        return std::filesystem::path(actual).lexically_normal() == expected.lexically_normal();
    };
    return (train.train_compiled_path.empty() || matches(train.train_compiled_path, directory / "train.bin")) &&
           (train.val_compiled_path.empty() || matches(train.val_compiled_path, directory / "val.bin")) &&
           (train.test_compiled_path.empty() || matches(train.test_compiled_path, directory / "test.bin"));
}

[[nodiscard]] std::filesystem::path normalized_artifact_path(const std::string_view path) {
    return std::filesystem::absolute(std::filesystem::path(std::string(path))).lexically_normal();
}

[[nodiscard]] bool is_managed_canonical_weight_path(const std::string_view path) {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path normalized = normalized_artifact_path(path);
    const std::filesystem::path cache_root =
        normalized_artifact_path(canonical_weight_cache_path(mmltk::rfdetr::kPresetCatalog.front().preset_name))
            .parent_path();
    if (normalized.parent_path() != cache_root) {
        return false;
    }
    const std::string filename = normalized.filename().string();
    return std::ranges::any_of(mmltk::rfdetr::kPresetCatalog,
                               [&](const auto& preset) { return filename == preset.canonical_weight_filename; });
}

[[nodiscard]] bool path_matches(const std::string_view left, const std::string_view right) {
    return !left.empty() && !right.empty() && normalized_artifact_path(left) == normalized_artifact_path(right);
}

[[nodiscard]] ModelInputMode model_input_mode(const mmltk::browser::CustomModelArtifactKind kind) noexcept {
    switch (kind) {
        case mmltk::browser::CustomModelArtifactKind::Weights:
            return ModelInputMode::Weights;
        case mmltk::browser::CustomModelArtifactKind::Onnx:
            return ModelInputMode::Onnx;
        case mmltk::browser::CustomModelArtifactKind::TensorRt:
            return ModelInputMode::TensorRt;
    }
    return ModelInputMode::Weights;
}

void assign_custom_model_path(const mmltk::browser::CustomModelArtifactKind kind, const std::string& path,
                              std::string& weights_path, std::string* onnx_path = nullptr,
                              std::string* tensorrt_path = nullptr) {
    weights_path.clear();
    if (onnx_path != nullptr) {
        onnx_path->clear();
    }
    if (tensorrt_path != nullptr) {
        tensorrt_path->clear();
    }
    switch (kind) {
        case mmltk::browser::CustomModelArtifactKind::Weights:
            weights_path = path;
            return;
        case mmltk::browser::CustomModelArtifactKind::Onnx:
            if (onnx_path != nullptr) {
                *onnx_path = path;
                return;
            }
            break;
        case mmltk::browser::CustomModelArtifactKind::TensorRt:
            if (tensorrt_path != nullptr) {
                *tensorrt_path = path;
                return;
            }
            break;
    }
    throw std::logic_error("custom model format is not supported by the routed workflow");
}

[[nodiscard]] bool custom_model_kind_allowed(const mmltk::browser::Workflow workflow,
                                             const mmltk::browser::CustomModelArtifactKind kind) noexcept {
    switch (workflow) {
        case mmltk::browser::Workflow::Train:
            return kind == mmltk::browser::CustomModelArtifactKind::Weights;
        case mmltk::browser::Workflow::Export:
            return kind != mmltk::browser::CustomModelArtifactKind::TensorRt;
        case mmltk::browser::Workflow::Validate:
        case mmltk::browser::Workflow::Predict:
        case mmltk::browser::Workflow::Annotate:
        case mmltk::browser::Workflow::Live:
            return true;
    }
    return false;
}

[[nodiscard]] mmltk::browser::Workflow normalized_model_workflow(const mmltk::browser::Workflow workflow) noexcept {
    return workflow == mmltk::browser::Workflow::Live ? mmltk::browser::Workflow::Predict : workflow;
}

struct ModelDialogAuthorization {
    std::uint64_t token = 0U;
    mmltk::browser::Workflow workflow = mmltk::browser::Workflow::Train;
    mmltk::browser::CustomModelArtifactKind artifact_kind = mmltk::browser::CustomModelArtifactKind::Weights;
    std::string dialog_id;
    bool defer_apply = false;
    bool consumed = false;
};

[[nodiscard]] std::optional<mmltk::browser::CustomModelArtifactKind> custom_model_kind_for_binding(
    const mmltk::browser::BrowserFileDialogBinding binding) noexcept {
    using Binding = mmltk::browser::BrowserFileDialogBinding;
    using Kind = mmltk::browser::CustomModelArtifactKind;
    switch (binding) {
        case Binding::TrainWeights:
        case Binding::PredictWeights:
        case Binding::AnnotateWeights:
        case Binding::ValidateWeights:
        case Binding::ExportWeights:
            return Kind::Weights;
        case Binding::PredictOnnx:
        case Binding::AnnotateOnnx:
        case Binding::ValidateOnnx:
        case Binding::ExportOnnx:
            return Kind::Onnx;
        case Binding::PredictTensorRt:
        case Binding::AnnotateTensorRt:
        case Binding::ValidateTensorRt:
            return Kind::TensorRt;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] const mmltk::browser::BrowserNativeFileDialogContractSpec* browser_dialog_spec(
    const std::string_view id) noexcept {
    const auto it = std::ranges::find(mmltk::browser::kBrowserNativeFileDialogs, id,
                                      &mmltk::browser::BrowserNativeFileDialogContractSpec::id);
    return it == mmltk::browser::kBrowserNativeFileDialogs.end() ? nullptr : &*it;
}

class ModelPreflightIncompatible final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

class ModelPreflightCancelled final : public std::runtime_error {
   public:
    ModelPreflightCancelled() : std::runtime_error("custom model preflight cancelled") {}
};

struct ModelPreflightTaskResult {
    bool compatible = false;
    std::string error;
};

void require_preflight_active(const std::atomic<bool>& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) {
        throw ModelPreflightCancelled{};
    }
}

[[nodiscard]] bool is_model_preflight_operational_error(const std::string_view message) noexcept {
    constexpr std::array operational_markers{
        std::string_view{"CUDA"},
        std::string_view{"cuda"},
        std::string_view{"cuDNN"},
        std::string_view{"CUDNN"},
        std::string_view{"driver"},
        std::string_view{"out of memory"},
        std::string_view{"failed to create TensorRT runtime"},
        std::string_view{"failed to create TensorRT builder"},
        std::string_view{"failed to create TensorRT build objects"},
        std::string_view{"TensorRT buildSerializedNetwork failed"},
        std::string_view{"failed to create TensorRT execution context"},
        std::string_view{"TensorRT enqueueV3 failed"},
    };
    return std::ranges::any_of(operational_markers, [message](const std::string_view marker) {
        return message.find(marker) != std::string_view::npos;
    });
}

template <typename Function>
decltype(auto) run_model_artifact_validation_step(Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const ModelPreflightCancelled&) {
        throw;
    } catch (const ModelPreflightIncompatible&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::system_error&) {
        throw;
    } catch (const std::exception& error) {
        if (is_model_preflight_operational_error(error.what())) {
            throw;
        }
        throw ModelPreflightIncompatible(error.what());
    }
}

void validate_preflight_outputs(const torch::Tensor& pred_logits, const torch::Tensor& pred_boxes) {
    if (!pred_logits.defined() || !pred_boxes.defined()) {
        throw ModelPreflightIncompatible("custom RF-DETR model produced incomplete outputs");
    }
    if (pred_logits.dim() != 3 || pred_boxes.dim() != 3 || pred_logits.size(0) != 1 || pred_boxes.size(0) != 1 ||
        pred_logits.size(1) <= 0 || pred_logits.size(1) != pred_boxes.size(1) || pred_logits.size(2) <= 0 ||
        pred_boxes.size(2) != 4) {
        throw ModelPreflightIncompatible("custom RF-DETR model produced incompatible output shapes");
    }
}

void run_custom_model_preflight(const mmltk::browser::CustomModelArtifactKind kind, const std::filesystem::path& path,
                                const std::string& preset_name, const int resolution, const int device_id,
                                const std::atomic<bool>& cancelled) {
    require_preflight_active(cancelled);
    mmltk::rfdetr::ModelArtifactRequest request;
    request.preset_name = preset_name;
    request.resolution = resolution;
    switch (kind) {
        case mmltk::browser::CustomModelArtifactKind::Weights:
            request.weights_path = path;
            break;
        case mmltk::browser::CustomModelArtifactKind::Onnx:
            request.onnx_path = path;
            break;
        case mmltk::browser::CustomModelArtifactKind::TensorRt:
            request.tensorrt_path = path;
            break;
    }
    mmltk::rfdetr::ResolvedModelArtifacts artifacts;
    try {
        artifacts = mmltk::rfdetr::resolve_model_artifacts(request);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::system_error&) {
        throw;
    } catch (const std::exception& error) {
        throw ModelPreflightIncompatible(error.what());
    }
    require_preflight_active(cancelled);
    const torch::Device device(torch::kCUDA, static_cast<c10::DeviceIndex>(device_id));
    c10::cuda::CUDAGuard device_guard(device);
    torch::NoGradGuard no_grad;
    const torch::Tensor input =
        torch::zeros({1, 3, resolution, resolution}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
    require_preflight_active(cancelled);
    if (kind == mmltk::browser::CustomModelArtifactKind::Weights) {
        mmltk::rfdetr::NativeRfDetrModel model(artifacts.config);
        run_model_artifact_validation_step([&model, &artifacts] { (void)model.load_weights(artifacts.weights_path); });
        model.eval();
        model.to(device);
        require_preflight_active(cancelled);
        const torch::Tensor mask =
            torch::zeros({1, resolution, resolution}, torch::TensorOptions().dtype(torch::kBool).device(device));
        const mmltk::rfdetr::ModelOutputs outputs = run_model_artifact_validation_step([&model, &input, &mask] {
            return model.forward(mmltk::rfdetr::NestedTensor{input, mask}, nullptr, false);
        });
        require_preflight_active(cancelled);
        validate_preflight_outputs(outputs.main.pred_logits, outputs.main.pred_boxes);
        (void)outputs.main.pred_logits.sum().cpu();
        return;
    }
    const std::string backend_name = kind == mmltk::browser::CustomModelArtifactKind::Onnx ? "onnx" : "tensorrt";
    std::unique_ptr<mmltk::rfdetr::InferenceBackend> backend =
        run_model_artifact_validation_step([&artifacts, &backend_name, device_id] {
            return mmltk::rfdetr::make_backend(artifacts, backend_name, device_id, true);
        });
    require_preflight_active(cancelled);
    const mmltk::rfdetr::OutputTensors outputs =
        run_model_artifact_validation_step([&backend, &input] { return backend->run(input); });
    require_preflight_active(cancelled);
    validate_preflight_outputs(outputs.pred_logits, outputs.pred_boxes);
    (void)outputs.pred_logits.sum().cpu();
}

}  

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
    void write_intent_trace(const mmltk::browser::RoutedIntent& routed, const nlohmann::json& payload) noexcept;
    void write_artifact_trace(std::string_view event, const nlohmann::json& fields) noexcept;

    View current_view_ = View::Train;
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
    std::string annotate_live_start_error_;
    AnnotateLiveStartupState annotate_live_startup_state_ = AnnotateLiveStartupState::Idle;
    std::chrono::steady_clock::time_point annotate_live_startup_deadline_{};
    std::shared_ptr<mmltk::live::LiveSessionController> live_controller_;
    std::unique_ptr<mmltk::live::LiveSessionStatus> live_session_status_;
    std::chrono::steady_clock::time_point next_live_pipeline_status_log_{};
    std::function<void(const mmltk::live::LiveSessionConfig&)> annotation_live_session_start_override_;
    bool annotation_live_session_running_for_testing_ = false;
    ActiveLiveMode active_live_mode_ = ActiveLiveMode::None;
    mmltk::runtime::UiCallbackQueue ui_callbacks_;
    mmltk::runtime::BackgroundExecutor background_executor_{2};
    BrowserArtifactController artifact_controller_;
    LivePredictController live_predict_controller_{};
    VastQueryController vast_query_controller_;
    RemoteTrainController remote_train_controller_;
    LocalTrainController local_train_controller_;
#if MMLTK_RFDETR_LIVE_CAPTURE
    std::shared_ptr<mmltk::live::WorkspaceSurfacePool> static_workspace_pool_;
    std::unique_ptr<mmltk::live::StaticWorkspaceCompositor> static_workspace_compositor_;
    bool static_workspace_start_failed_ = false;
#endif
    std::string live_static_preview_source_name_;
    std::string live_preview_error_;
    bool live_preview_fit_to_capture_ = false;
    bool live_crop_overlay_mode_ = false;
    bool shutting_down_ = false;
    PreviewRectDragSession live_crop_drag_session_{};
    FileBrowseField active_file_browse_field_ = FileBrowseField::None;
    std::string active_browser_dialog_id_;
    std::string active_browser_dialog_title_;
    mmltk::browser::FileDialogState browser_file_dialog_state_;
    std::optional<ModelDialogAuthorization> model_dialog_authorization_;
    mmltk::browser::ModelPreflightState model_preflight_state_;
    std::shared_ptr<std::atomic<bool>> model_preflight_cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::mutex> model_preflight_serialization_ = std::make_shared<std::mutex>();
    std::string picker_error_;
    std::string artifact_dataset_signature_;
    std::string artifact_compiled_directory_;
    std::string artifact_weight_preset_;
    std::string auto_weight_path_;
    std::string vast_api_key_;
    std::FILE* intent_trace_file_ = nullptr;
    std::atomic<bool> intent_trace_enabled_{false};
    std::mutex intent_trace_mutex_;
    mmltk::runtime::ModelRegistry model_registry_;
    GuiSettingsPersistence settings_persistence_;
    std::chrono::steady_clock::time_point next_settings_persistence_poll_{};
    std::unique_ptr<AnnotationWorkflowCoordinator> annotation_workflow_coordinator_;
    std::optional<BrowserWorkspaceViewportState> browser_workspace_viewport_state_;
    std::optional<BrowserWorkspaceBoundsState> browser_workspace_bounds_state_;
};

namespace {

struct BrowserPreviewSnapshot {
    bool initialized = false;
    bool has_frame = false;
    mmltk::live::LiveCaptureRegion displayed_region{};
    std::uint64_t last_frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    std::string last_error;
    bool interop_failed = false;
};

}  

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
    bool publish_live_manual_overlay();
    bool request_save_now();
    void start_live_session();
    void stop_live_session();
    [[nodiscard]] bool live_running() const;

   private:
    bool dispatch_save_now(const AnnotationSaveTabPlan& save_tab_plan);
    [[nodiscard]] bool ensure_live_annotation_frame_pixels(std::string* error_message) const;
    [[nodiscard]] std::optional<AnnotationBox> crop_overlay_box() const;
    [[nodiscard]] AnnotationWorkflowSaveRequest make_save_request() const;
    void reset_live_session_state(bool clear_live_preview_error, App::ActiveLiveMode active_mode);
    void log_annotation_resolve_error(const char* error_context, const std::string& error) const;
    [[nodiscard]] std::optional<AnnotationSaveSnapshot> make_current_save_snapshot(bool refresh_live_frame,
                                                                                   const char* resolve_error_context);
    [[nodiscard]] bool launch_save(AnnotationSaveSnapshot save_snapshot, bool live_mode);
    void ensure_static_workspace() noexcept;

    App& app_;
};

App::Impl::Impl(std::string vast_api_key, std::string settings_path)
    : artifact_controller_(background_executor_, ui_callbacks_),
      vast_query_controller_(background_executor_, ui_callbacks_),
      remote_train_controller_(background_executor_, ui_callbacks_),
      local_train_controller_(background_executor_, ui_callbacks_),
      vast_api_key_(std::move(vast_api_key)),
      settings_persistence_(std::move(settings_path)) {
    if (const char* trace_path = std::getenv("MMLTK_GUI_TRACE_FILE"); trace_path != nullptr && *trace_path != '\0') {
        intent_trace_file_ = std::fopen(trace_path, "a");
        if (intent_trace_file_ == nullptr) {
            mmltk::logging::logger("gui")->warn("failed to open GUI JSONL trace `{}`: {}", trace_path,
                                                std::strerror(errno));
        } else {
            intent_trace_enabled_.store(true, std::memory_order_release);
            artifact_controller_.set_trace_sink([this](const std::string_view event, const nlohmann::json& fields) {
                write_artifact_trace(event, fields);
            });
        }
    }
}

App::Impl::~Impl() {
    intent_trace_enabled_.store(false, std::memory_order_release);
    artifact_controller_.set_trace_sink({});
    std::lock_guard lock(intent_trace_mutex_);
    if (intent_trace_file_ != nullptr) {
        std::fclose(intent_trace_file_);
        intent_trace_file_ = nullptr;
    }
}

void App::Impl::write_intent_trace(const mmltk::browser::RoutedIntent& routed, const nlohmann::json& payload) noexcept {
    if (!intent_trace_enabled_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock(intent_trace_mutex_);
    if (intent_trace_file_ == nullptr) {
        return;
    }
    try {
        nlohmann::json record;
        if (const auto* ui_log = std::get_if<mmltk::browser::UiLogIntent>(&routed.payload); ui_log != nullptr) {
            nlohmann::json fields = ui_log->fields;
            if (ui_log->event == "workspace.encoded") {
                fields["host_receive_ns"] = mmltk::live::live_steady_clock_now_ns();
            }
            record = {
                {"source", "ui"},
                {"level", ui_log->level},
                {"event", ui_log->event},
                {"fields", std::move(fields)},
                {"request_id", routed.request_id},
                {"workflow", std::string(mmltk::browser::workflow_name(routed.workflow))},
            };
        } else {
            record = {
                {"source", "host"},
                {"level", "trace"},
                {"event", "intent"},
                {"request_id", routed.request_id},
                {"workflow", std::string(mmltk::browser::workflow_name(routed.workflow))},
                {"intent", routed.intent},
                {"payload", payload},
            };
        }
        const std::string line = record.dump();
        std::fwrite(line.data(), sizeof(char), line.size(), intent_trace_file_);
        std::fputc('\n', intent_trace_file_);
        std::fflush(intent_trace_file_);
    } catch (...) {
        std::fputs("{\"source\":\"host\",\"level\":\"error\",\"event\":\"trace.serialize_failed\"}\n",
                   intent_trace_file_);
        std::fflush(intent_trace_file_);
    }
}

void App::Impl::write_artifact_trace(const std::string_view event, const nlohmann::json& fields) noexcept {
    if (!intent_trace_enabled_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock(intent_trace_mutex_);
    if (intent_trace_file_ == nullptr) {
        return;
    }
    try {
        const nlohmann::json record{
            {"source", "host"},
            {"level", "trace"},
            {"event", std::string(event)},
            {"fields", fields},
        };
        const std::string line = record.dump();
        std::fwrite(line.data(), sizeof(char), line.size(), intent_trace_file_);
        std::fputc('\n', intent_trace_file_);
        std::fflush(intent_trace_file_);
    } catch (...) {
        std::fputs("{\"source\":\"host\",\"level\":\"error\",\"event\":\"artifact.trace.serialize_failed\"}\n",
                   intent_trace_file_);
        std::fflush(intent_trace_file_);
    }
}

void App::Impl::initialize(App& app) {
    model_registry_.register_module(make_rfdetr_model_module());
    apply_default_gui_state(train_, validate_, predict_, annotate_, export_);
    bool loaded_settings = false;
    {
        GuiSettingsSnapshot snap = app.make_gui_settings_snapshot();
        if (settings_persistence_.load(snap)) {
            loaded_settings = true;
            current_view_ = snap.current_view;
        }
    }
    ui_settings_.content_scale = 1.0f;
    annotate_brush_radius_ = std::clamp(ui_settings_.annotation_brush_radius, 1, 128);

    for (const View workflow_view : {View::Train, View::Validate, View::Predict, View::Annotate, View::Export}) {
        const WorkflowModelSelectionState& selection =
            workflow_model_selection(workflow_view, train_, validate_, predict_, annotate_, export_);
        if (find_model_preset(selection.preset_name) == nullptr) {
            app.apply_workflow_preset_defaults(workflow_view, kDefaultModelPresetName);
        }
    }
    if (loaded_settings && train_.use_compiled_directory_defaults && !split_paths_match_directory_defaults(train_)) {
        train_.use_compiled_directory_defaults = false;
    }
    local_train_controller_.initialize(train_.local_device_ids);
    app.refresh_browser_artifacts();

    (void)annotate_controller_.set_active_tool(AnnotationToolKind::Direct, annotate_document_, annotate_session_);
    annotation_workflow_coordinator_ = std::make_unique<AnnotationWorkflowCoordinator>(app);
    if (current_view_ == View::Annotate) {
        annotation_workflow_coordinator_->prepare_source();
    }
}

#define current_view_ impl_->current_view_
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
#define annotate_live_start_error_ impl_->annotate_live_start_error_
#define annotate_live_startup_state_ impl_->annotate_live_startup_state_
#define annotate_live_startup_deadline_ impl_->annotate_live_startup_deadline_
#define live_controller_ impl_->live_controller_
#define live_session_status_ impl_->live_session_status_
#define annotation_live_session_running_for_testing_ impl_->annotation_live_session_running_for_testing_
#define active_live_mode_ impl_->active_live_mode_
#define ui_callbacks_ impl_->ui_callbacks_
#define background_executor_ impl_->background_executor_
#define artifact_controller_ impl_->artifact_controller_
#define live_predict_controller_ impl_->live_predict_controller_
#define vast_query_controller_ impl_->vast_query_controller_
#define remote_train_controller_ impl_->remote_train_controller_
#define local_train_controller_ impl_->local_train_controller_
#define static_workspace_pool_ impl_->static_workspace_pool_
#define static_workspace_compositor_ impl_->static_workspace_compositor_
#define static_workspace_start_failed_ impl_->static_workspace_start_failed_
#define live_static_preview_source_name_ impl_->live_static_preview_source_name_
#define live_preview_error_ impl_->live_preview_error_
#define live_preview_fit_to_capture_ impl_->live_preview_fit_to_capture_
#define live_crop_overlay_mode_ impl_->live_crop_overlay_mode_
#define shutting_down_ impl_->shutting_down_
#define live_crop_drag_session_ impl_->live_crop_drag_session_
#define active_file_browse_field_ impl_->active_file_browse_field_
#define active_browser_dialog_id_ impl_->active_browser_dialog_id_
#define active_browser_dialog_title_ impl_->active_browser_dialog_title_
#define browser_file_dialog_state_ impl_->browser_file_dialog_state_
#define model_dialog_authorization_ impl_->model_dialog_authorization_
#define model_preflight_state_ impl_->model_preflight_state_
#define model_preflight_cancel_ impl_->model_preflight_cancel_
#define model_preflight_serialization_ impl_->model_preflight_serialization_
#define picker_error_ impl_->picker_error_
#define artifact_dataset_signature_ impl_->artifact_dataset_signature_
#define artifact_compiled_directory_ impl_->artifact_compiled_directory_
#define artifact_weight_preset_ impl_->artifact_weight_preset_
#define auto_weight_path_ impl_->auto_weight_path_
#define vast_api_key_ impl_->vast_api_key_
#define model_registry_ impl_->model_registry_
#define settings_persistence_ impl_->settings_persistence_
#define annotation_workflow_coordinator_ impl_->annotation_workflow_coordinator_
#define browser_workspace_viewport_state_ impl_->browser_workspace_viewport_state_
#define browser_workspace_bounds_state_ impl_->browser_workspace_bounds_state_

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
    ui_callbacks_.set_wake_callback(std::move(callback));
}

void App::shutdown() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    set_evented_work_wake_callback({});
    browser_workspace_viewport_state_.reset();
    browser_workspace_bounds_state_.reset();
    stop_live_predict_session();
    annotation_workflow_coordinator_->shutdown();
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (static_workspace_compositor_ != nullptr) {
        static_workspace_compositor_->stop();
        static_workspace_compositor_.reset();
        static_workspace_pool_.reset();
    }
#endif
    local_train_controller_.shutdown();
    model_preflight_cancel_->store(true, std::memory_order_relaxed);
    artifact_controller_.shutdown();
    settings_persistence_.flush();
    background_executor_.wait_idle();
    ui_callbacks_.drain();
    artifact_controller_.set_trace_sink({});
}

std::shared_ptr<mmltk::live::LiveSessionController> App::active_live_controller_handle() const {
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (active_live_mode_ == ActiveLiveMode::Annotate) {
        return live_controller_;
    }
    if (active_live_mode_ == ActiveLiveMode::Predict) {
        return live_predict_controller_.controller_handle();
    }
#endif
    return {};
}

std::shared_ptr<mmltk::live::WorkspaceSurfacePool> App::active_workspace_surface_pool_handle() const {
#if MMLTK_RFDETR_LIVE_CAPTURE
    const std::shared_ptr<mmltk::live::LiveSessionController> controller = active_live_controller_handle();
    if (controller != nullptr) {
        return controller->workspace_surface_pool_handle();
    }
    return static_workspace_pool_;
#else
    return {};
#endif
}

std::optional<BrowserWorkspaceViewportState> App::browser_workspace_viewport_state() const {
    return browser_workspace_viewport_state_;
}

std::optional<BrowserWorkspaceBoundsState> App::browser_workspace_bounds_state() const {
    return browser_workspace_bounds_state_;
}

GuiSettingsSnapshot App::make_gui_settings_snapshot() {
    return GuiSettingsSnapshot{current_view_, &ui_settings_, &train_, &validate_, &predict_, &annotate_, &export_};
}

mmltk::browser::StateSnapshot App::browser_state_snapshot() {
    GuiSettingsSnapshot gui_snapshot = make_gui_settings_snapshot();
    const LocalTrainSessionState& local_train_state = local_train_controller_.session_state();
    const RemoteTrainSessionState& remote_train_state = remote_train_controller_.state();
    const bool static_preview_visible = !live_predict_running() && !live_static_preview_source_name_.empty();

    BrowserPreviewSnapshot preview_state;
    preview_state.initialized = true;

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
    mmltk::browser::StateSnapshot snapshot = mmltk::browser::make_state_snapshot(inputs);
    snapshot.runtime_capabilities.host_backend = mmltk::browser::BrowserHostBackend::Firefox;
    snapshot.runtime_capabilities.workspace_surface_bridge =
        mmltk::browser::BrowserRuntimeCapabilityStatus::Unavailable;
    snapshot.runtime_capabilities.workspace_surface_zero_copy =
        mmltk::browser::BrowserRuntimeCapabilityStatus::Unavailable;
    snapshot.workspace_surface.reset();
    {
        const WorkflowModelSelectionState& model_selection =
            workflow_model_selection(current_view_, train_, validate_, predict_, annotate_, export_);
        std::string resolved_preset_name = model_selection.preset_name;
        const mmltk::rfdetr::ModelPresetConfig& active_preset = resolve_model_preset(resolved_preset_name);
        const auto* active_module = model_registry_.find_module_for_preset(resolved_preset_name);
        nlohmann::json preset_options = nlohmann::json::array();
        for (const auto& preset : model_registry_.presets()) {
            preset_options.push_back({
                {"module_id", preset.module_id},
                {"preset_name", preset.preset_name},
                {"display_name", preset.display_name},
                {"size_label", preset.size_label},
                {"task", preset.task},
                {"resolution", preset.resolution},
                {"canonical_weight_filename", preset.canonical_weight_filename},
                {"selected",
                 model_selection.model_input != ModelInputMode::None && preset.preset_name == resolved_preset_name},
            });
        }
        snapshot.workflow_state["model_config"] = {
            {"preset_name", resolved_preset_name},
            {"module_id", active_module != nullptr ? std::string(active_module->module_id()) : std::string{}},
            {"module_label", active_module != nullptr ? std::string(active_module->display_name()) : std::string{}},
            {"canonical_weight_filename", std::string(active_preset.canonical_weight_filename)},
            {"resolution", model_selection.model_resolution},
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
    snapshot.artifacts = artifact_controller_.snapshot();
    snapshot.file_dialog = browser_file_dialog_state_;
    snapshot.model_preflight = model_preflight_state_;
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
    const bool annotate_live_capture_supported = live_capture_supported();
    const std::string annotate_live_source_error = annotate_live_video && annotate_live_capture_supported
                                                       ? validate_video_stream_source(annotate_.source)
                                                       : std::string{};
    const bool annotate_live_ready = annotate_live_video && annotate_live_source_error.empty() &&
                                     !annotate_live_running_now && !annotate_block_actions &&
                                     annotate_live_capture_supported;
    const AnnotationObject* selected_annotation_object =
        mmltk::gui::selected_object(annotate_document_, annotate_session_);
    const bool selected_supports_mask_editing =
        selected_annotation_object != nullptr && annotation_object_supports_mask_editing(*selected_annotation_object);
    const bool annotate_brush_enabled =
        selected_supports_mask_editing && annotation_tool_kind_uses_brush(annotate_session_.active_tool());
    snapshot.workflow_state["annotate_runtime"] = {
        {"setup",
         {{"live_video_source", annotate_live_video},
          {"live_capture_supported", annotate_live_capture_supported},
          {"live_running", annotate_live_running_now},
          {"block_actions", annotate_block_actions},
          {"prepare_running", annotate_prepare_running_},
          {"frame_load_running", annotate_frame_load_running_},
          {"has_inputs", annotate_has_inputs},
          {"live_source_error", annotate_live_source_error},
          {"current_input_index",
           annotate_has_inputs ? nlohmann::json(annotate_current_input_index_) : nlohmann::json(nullptr)},
          {"input_count", annotate_inputs_.size()},
          {"can_start_live", annotate_live_ready},
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
        !annotate_live_video ? "Live annotate is only available for video-stream sources."
        : annotate_live_startup_state_ == AnnotateLiveStartupState::Starting ? "Live annotate is starting."
        : annotate_live_startup_state_ == AnnotateLiveStartupState::Failed || !annotate_live_start_error_.empty()
            ? "Live annotate failed: " + annotate_live_start_error_
        : annotate_live_startup_state_ == AnnotateLiveStartupState::Running || annotate_live_running_now
            ? "Live annotate is streaming from the current capture device."
        : !annotate_live_capture_supported    ? "Live annotate is unavailable in this build."
        : !annotate_live_source_error.empty() ? annotate_live_source_error
                                              : "Start live annotate to keep the workspace bound to incoming frames.";
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
           {{"enabled", annotate_live_ready},
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
            {"payload_key", "enabled"}}}}},
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
    live_runtime.startup_state = active_live_mode_ == ActiveLiveMode::Annotate || !annotate_live_start_error_.empty()
                                     ? annotate_live_startup_state_name(annotate_live_startup_state_)
                                     : "idle";
    live_runtime.starting =
        live_predict_controller_.starting() || (active_live_mode_ == ActiveLiveMode::Annotate &&
                                                annotate_live_startup_state_ == AnnotateLiveStartupState::Starting);
    live_runtime.stopping = live_predict_controller_.stopping();
    live_runtime.show_static_preview = static_preview_visible;
    live_runtime.show_idle_start_error = !live_predict_running() && (!live_predict_controller_.start_error().empty() ||
                                                                     !annotate_live_start_error_.empty());
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
    live_runtime.start_error =
        annotate_live_start_error_.empty() ? live_predict_controller_.start_error() : annotate_live_start_error_;
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
        if (live_controller_ != nullptr) {
            active_live_status = live_controller_->snapshot_status();
            if (!live_session_status_) {
                live_session_status_ = std::make_unique<mmltk::live::LiveSessionStatus>();
            }
            *live_session_status_ = *active_live_status;
            refresh_annotate_live_startup_state(*active_live_status);
        } else if (live_session_status_ != nullptr) {
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
        live_runtime.fanout.running = status.fanout.running;
        live_runtime.fanout.frames_fanned_out = status.fanout.frames_fanned_out;
        live_runtime.fanout.skipped_detect_publishes = status.fanout.skipped_detect_publishes;
        live_runtime.fanout.skipped_output_publishes = status.fanout.skipped_output_publishes;
        live_runtime.fanout.release_backlog = status.fanout.release_backlog;
        live_runtime.fanout.acquire_misses = status.fanout.acquire_misses;
        live_runtime.fanout.last_error = status.fanout.last_error;
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
        live_runtime.compositor.frames_composited_after_startup = status.compositor.frames_composited_after_startup;
        live_runtime.compositor.frames_dropped = status.compositor.frames_dropped;
        live_runtime.compositor.skipped_compositor_presents = status.compositor.skipped_compositor_presents;
        live_runtime.compositor.source_acquire_waits = status.compositor.source_acquire_waits;
        live_runtime.compositor.source_leases_acquired = status.compositor.source_leases_acquired;
        live_runtime.compositor.source_leases_released = status.compositor.source_leases_released;
        live_runtime.compositor.source_stale_releases = status.compositor.source_stale_releases;
        live_runtime.compositor.source_skipped_stale_frames = status.compositor.source_skipped_stale_frames;
        live_runtime.compositor.source_slot_pressure = status.compositor.source_slot_pressure;
        live_runtime.compositor.source_release_latency_ns = status.compositor.source_release_latency_ns;
        live_runtime.compositor.front_slot_index = status.compositor.front_slot_index;
        live_runtime.compositor.front_slot_revision = status.compositor.front_slot_revision;
        live_runtime.compositor.last_frame_id = status.compositor.last_frame_id;
        live_runtime.compositor.manual_overlay_active = status.compositor.manual_overlay_active;
        live_runtime.compositor.analysis_overlay_active = status.compositor.analysis_overlay_active;
        live_runtime.compositor.last_error = status.compositor.last_error;
    }
#endif
    live_runtime.startup_state = active_live_mode_ == ActiveLiveMode::Annotate || !annotate_live_start_error_.empty()
                                     ? annotate_live_startup_state_name(annotate_live_startup_state_)
                                     : "idle";
    live_runtime.starting =
        live_predict_controller_.starting() || (active_live_mode_ == ActiveLiveMode::Annotate &&
                                                annotate_live_startup_state_ == AnnotateLiveStartupState::Starting);
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
    live_preview_json["surface_revision"] = "";
    live_preview_json["owner"] = "none";
    live_preview_json["publish_ns"] = 0U;
    live_preview_json["native_presented"] = false;
    live_preview_json["display_startup_state"] = "frame unavailable";
    live_preview_json["frame_available"] = false;
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
                        *annotate_frame_, annotate_, annotate_.preset_name, &annotate_assist_error_);
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
    impl_->write_intent_trace(routed, intent.payload);
    const auto make_gui_snapshot = [this]() { return make_gui_settings_snapshot(); };
    const auto apply_settings_patch = [this, &make_gui_snapshot](const nlohmann::json& patch) {
        if (!patch.is_object() || patch.empty()) {
            return;
        }
        const nlohmann::json* train_dataset_patch = nullptr;
        std::vector<std::pair<View, std::string>> preset_patches;
        preset_patches.reserve(5U);
        if (const auto workflows = patch.find("workflows"); workflows != patch.end() && workflows->is_object()) {
            const auto collect_preset_patch = [&workflows, &preset_patches](const char* workflow_key,
                                                                            const View workflow_view) {
                const auto workflow = workflows->find(workflow_key);
                if (workflow == workflows->end() || !workflow->is_object()) {
                    return;
                }
                const auto artifacts = workflow->find("model_artifacts");
                if (artifacts == workflow->end() || !artifacts->is_object()) {
                    return;
                }
                const auto preset = artifacts->find("preset_name");
                if (preset != artifacts->end() && preset->is_string()) {
                    preset_patches.emplace_back(workflow_view, preset->get<std::string>());
                }
            };
            collect_preset_patch("train", View::Train);
            collect_preset_patch("validate", View::Validate);
            collect_preset_patch("predict", View::Predict);
            collect_preset_patch("annotate", View::Annotate);
            collect_preset_patch("export", View::Export);
            if (const auto train = workflows->find("train"); train != workflows->end() && train->is_object()) {
                if (const auto dataset = train->find("dataset_paths");
                    dataset != train->end() && dataset->is_object()) {
                    train_dataset_patch = &*dataset;
                }
            }
        }
        const bool compiled_directory_patched =
            train_dataset_patch != nullptr && train_dataset_patch->contains("compiled_directory");
        const bool split_override_patched =
            train_dataset_patch != nullptr &&
            (train_dataset_patch->contains("train_compiled_path") ||
             train_dataset_patch->contains("val_compiled_path") || train_dataset_patch->contains("test_compiled_path"));
        const bool defaults_mode_patched =
            train_dataset_patch != nullptr && train_dataset_patch->contains("use_compiled_directory_defaults");

        const mmltk::browser::Workflow active_workflow =
            normalized_model_workflow(mmltk::browser::workflow_from_view(current_view_));
        for (const auto& [workflow_view, preset_name] : preset_patches) {
            const mmltk::browser::Workflow patched_workflow =
                normalized_model_workflow(mmltk::browser::workflow_from_view(workflow_view));
            const bool active = patched_workflow == active_workflow;
            if (active) {
                artifact_controller_.cancel_weight_acquisition();
                artifact_weight_preset_.clear();
                auto_weight_path_.clear();
            }
            apply_workflow_preset_defaults(workflow_view, preset_name);
            if (impl_->intent_trace_enabled_.load(std::memory_order_acquire)) {
                impl_->write_artifact_trace("weight.selection_applied",
                                            {{"preset_name", preset_name},
                                             {"workflow", std::string(mmltk::browser::workflow_name(patched_workflow))},
                                             {"active", active}});
            }
        }
        GuiSettingsSnapshot gui_snapshot = make_gui_snapshot();
        mmltk::browser::apply_settings_update(mmltk::browser::SettingsUpdateIntent{patch}, gui_snapshot);
        current_view_ = gui_snapshot.current_view;
        annotate_brush_radius_ = std::clamp(ui_settings_.annotation_brush_radius, 1, 128);
        local_train_controller_.set_selected_device_ids(train_.local_device_ids);
        if (!defaults_mode_patched && split_override_patched) {
            train_.use_compiled_directory_defaults = false;
        } else if (!defaults_mode_patched && compiled_directory_patched) {
            train_.use_compiled_directory_defaults = true;
        }
        if (train_.use_compiled_directory_defaults && (compiled_directory_patched || defaults_mode_patched)) {
            infer_compiled_dataset_paths();
            artifact_compiled_directory_ = train_.compiled_dataset_dir;
        }
        refresh_browser_artifacts();
        if (current_view_ == View::Annotate) {
            annotation_workflow_coordinator_->prepare_source();
        }
    };
    const auto require_custom_model_ready =
        [this](const mmltk::browser::Workflow workflow, const WorkflowModelSelectionState& selection,
               const std::string& weights_path, const std::string& onnx_path, const std::string& tensorrt_path) {
            if (selection.model_source != ModelSelectionSource::Custom) {
                return;
            }
            const std::string* selected_path = &weights_path;
            mmltk::browser::CustomModelArtifactKind kind = mmltk::browser::CustomModelArtifactKind::Weights;
            if (selection.model_input == ModelInputMode::Onnx) {
                selected_path = &onnx_path;
                kind = mmltk::browser::CustomModelArtifactKind::Onnx;
            } else if (selection.model_input == ModelInputMode::TensorRt) {
                selected_path = &tensorrt_path;
                kind = mmltk::browser::CustomModelArtifactKind::TensorRt;
            }
            const bool ready =
                model_preflight_state_.status == mmltk::browser::ModelPreflightStatus::Ready &&
                model_preflight_state_.workflow == normalized_model_workflow(workflow) &&
                model_preflight_state_.artifact_kind == kind &&
                model_preflight_state_.preset_name == selection.preset_name &&
                model_preflight_state_.resolution == static_cast<std::uint32_t>(selection.model_resolution) &&
                path_matches(model_preflight_state_.path, *selected_path);
            if (!ready) {
                throw std::runtime_error(
                    "workflow launch blocked until the custom RF-DETR artifact passes its synthetic-image preflight: " +
                    (model_preflight_state_.error.empty() ? std::string("verification is still running")
                                                          : model_preflight_state_.error));
            }
        };
    const auto apply_routed_host_intent = [this, &routed, &make_gui_snapshot]() {
        GuiSettingsSnapshot gui_snapshot = make_gui_snapshot();
        mmltk::browser::apply_routed_intent(
            routed, mmltk::browser::BrowserHostIntentContext{
                        &current_view_,
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

    if (std::holds_alternative<mmltk::browser::UiLogIntent>(routed.payload)) {
        return;
    }

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
            annotate_session_.touch_overlay();
            invalidate_annotation_preview();
        }
        return;
    }

    if (std::holds_alternative<mmltk::browser::ToolSelectIntent>(routed.payload)) {
        apply_routed_host_intent();
        cancel_annotation_canvas_interactions();
        annotate_session_.touch_overlay();
        invalidate_annotation_preview();
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
                    "browser live.preview.full_frame_display requires a running live "
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
        std::holds_alternative<mmltk::browser::AnnotateWorkspacePointerIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceFillIntent>(routed.payload) ||
        std::holds_alternative<mmltk::browser::AnnotateWorkspaceColorSampleIntent>(routed.payload)) {
        const std::uint64_t document_generation = annotate_document_.generation();
        const std::uint64_t overlay_revision = annotate_session_.overlay_revision();
        // cppcheck-suppress redundantCopyLocalConst
        const PreviewRectDragSession create_drag = annotate_session_.create_drag_session();
        // cppcheck-suppress redundantCopyLocalConst
        const PreviewRectDragSession crop_drag = annotate_session_.crop_drag_session();
        // cppcheck-suppress redundantCopyLocalConst
        const PreviewRectDragSession direct_drag = annotate_session_.direct_drag_session();
        apply_routed_host_intent();
        const bool drag_changed =
            !preview_rect_drag_sessions_equal(create_drag, annotate_session_.create_drag_session()) ||
            !preview_rect_drag_sessions_equal(crop_drag, annotate_session_.crop_drag_session()) ||
            !preview_rect_drag_sessions_equal(direct_drag, annotate_session_.direct_drag_session());
        if (drag_changed && annotate_session_.overlay_revision() == overlay_revision) {
            annotate_session_.touch_overlay();
        }
        if (annotate_document_.generation() != document_generation ||
            annotate_session_.overlay_revision() != overlay_revision) {
            invalidate_annotation_preview();
        }
        return;
    }

    if (std::holds_alternative<mmltk::browser::TrainLocalGpuRefreshIntent>(routed.payload)) {
        current_view_ = View::Train;
        local_train_controller_.refresh_visible_gpus(train_.local_device_ids);
        return;
    }

    if (const auto* compile = std::get_if<mmltk::browser::DatasetCompileStartIntent>(&routed.payload);
        compile != nullptr) {
        if (compile->resolution == 0U ||
            compile->resolution > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("dataset compile resolution is outside the supported range");
        }
        current_view_ = View::Train;
        train_.dataset_source_dir = compile->source_dir;
        train_.compiled_dataset_dir = compile->output_dir;
        train_.model_resolution = static_cast<int>(compile->resolution);
        train_.overwrite_compiled_dataset = compile->overwrite;
        artifact_controller_.compile_dataset(compile->source_dir, compile->output_dir, train_.preset_name,
                                             compile->resolution, compile->overwrite,
                                             [this](const std::string& output_dir) {
                                                 train_.compiled_dataset_dir = output_dir;
                                                 train_.use_compiled_directory_defaults = true;
                                                 artifact_compiled_directory_.clear();
                                                 artifact_dataset_signature_.clear();
                                                 refresh_browser_artifacts();
                                             });
        return;
    }

    if (std::holds_alternative<mmltk::browser::DatasetCompileCancelIntent>(routed.payload)) {
        artifact_controller_.cancel_dataset_compile();
        return;
    }

    if (const auto* reset = std::get_if<mmltk::browser::SettingsResetIntent>(&routed.payload); reset != nullptr) {
        if (!reset->confirmed) {
            throw std::runtime_error("settings reset requires explicit confirmation");
        }
        const mmltk::browser::ArtifactState artifact_state = artifact_controller_.snapshot();
        if (job_.running || live_predict_running() || annotation_live_running() || annotate_assist_running_ ||
            annotate_save_running_ || local_train_controller_.session_state().running ||
            remote_train_controller_.state().request_running || active_file_browse_field_ != FileBrowseField::None ||
            artifact_state.dataset.compiling ||
            model_preflight_state_.status == mmltk::browser::ModelPreflightStatus::Verifying) {
            throw std::runtime_error(
                "settings reset is blocked while a workflow job, dataset compilation, model verification, or native "
                "dialog is active");
        }
        artifact_controller_.cancel_weight_acquisition();
        model_preflight_cancel_->store(true, std::memory_order_relaxed);
        model_preflight_cancel_ = std::make_shared<std::atomic<bool>>(false);
        ui_settings_ = UiSettingsState{};
        annotate_brush_radius_ = ui_settings_.annotation_brush_radius;
        apply_default_gui_state(train_, validate_, predict_, annotate_, export_);
        artifact_dataset_signature_.clear();
        artifact_compiled_directory_.clear();
        artifact_weight_preset_.clear();
        auto_weight_path_.clear();
        browser_file_dialog_state_ = {};
        model_dialog_authorization_.reset();
        model_preflight_state_ = {};
        refresh_browser_artifacts();
        GuiSettingsSnapshot reset_snapshot = make_gui_settings_snapshot();
        settings_persistence_.notify_frame(reset_snapshot);
        settings_persistence_.flush();
        return;
    }

    if (const auto* custom = std::get_if<mmltk::browser::CustomModelSelectIntent>(&routed.payload); custom != nullptr) {
        if (!custom_model_kind_allowed(routed.workflow, custom->artifact_kind)) {
            throw std::runtime_error("selected custom model format is not supported by this workflow");
        }
        auto& authorization = model_dialog_authorization_;
        if (custom->dialog_token == 0U || !authorization.has_value() || authorization->consumed ||
            !authorization->defer_apply || custom->dialog_token != authorization->token ||
            normalized_model_workflow(routed.workflow) != authorization->workflow ||
            custom->artifact_kind != authorization->artifact_kind ||
            authorization->dialog_id != browser_file_dialog_state_.dialog_id ||
            custom->dialog_token != browser_file_dialog_state_.token ||
            browser_file_dialog_state_.status != mmltk::browser::FileDialogResultStatus::Selected ||
            !path_matches(custom->path, browser_file_dialog_state_.path)) {
            throw std::runtime_error("custom model selection does not match the completed native file dialog");
        }
        if (mmltk::rfdetr::find_model_preset(custom->preset_name) == nullptr) {
            throw std::runtime_error("unknown RF-DETR preset for custom model selection: " + custom->preset_name);
        }
        if (custom->resolution == 0U ||
            custom->resolution > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("custom model resolution is outside the supported range");
        }
        if (!std::filesystem::is_regular_file(custom->path)) {
            throw std::runtime_error("custom model path is not a readable regular file: " + custom->path);
        }
        authorization->consumed = true;

        const int resolution = static_cast<int>(custom->resolution);
        const ModelInputMode input = model_input_mode(custom->artifact_kind);
        int device_id = 0;
        const auto assign_selection = [&](WorkflowModelSelectionState& selection) {
            selection.preset_name = custom->preset_name;
            selection.model_resolution = resolution;
            selection.model_source = ModelSelectionSource::Custom;
            selection.model_input = input;
        };
        switch (routed.workflow) {
            case mmltk::browser::Workflow::Train:
                assign_selection(train_);
                assign_custom_model_path(custom->artifact_kind, custom->path, train_.weights_path);
                train_.input_mode = TrainInputMode::Weights;
                device_id = train_.local_device_ids.empty() ? 0 : train_.local_device_ids.front();
                break;
            case mmltk::browser::Workflow::Validate:
                assign_selection(validate_);
                assign_custom_model_path(custom->artifact_kind, custom->path, validate_.weights_path,
                                         &validate_.onnx_path, &validate_.tensorrt_path);
                device_id = validate_.device_id;
                break;
            case mmltk::browser::Workflow::Predict:
            case mmltk::browser::Workflow::Live:
                assign_selection(predict_);
                assign_custom_model_path(custom->artifact_kind, custom->path, predict_.weights_path,
                                         &predict_.onnx_path, &predict_.tensorrt_path);
                device_id = predict_.device_id;
                break;
            case mmltk::browser::Workflow::Annotate:
                assign_selection(annotate_);
                assign_custom_model_path(custom->artifact_kind, custom->path, annotate_.weights_path,
                                         &annotate_.onnx_path, &annotate_.tensorrt_path);
                device_id = annotate_.device_id;
                break;
            case mmltk::browser::Workflow::Export:
                assign_selection(export_);
                assign_custom_model_path(custom->artifact_kind, custom->path, export_.weights_path, &export_.onnx_path);
                device_id = export_.device_id;
                break;
        }
        artifact_controller_.cancel_weight_acquisition();
        artifact_weight_preset_.clear();
        auto_weight_path_.clear();
        const mmltk::browser::Workflow current_model_workflow =
            normalized_model_workflow(mmltk::browser::workflow_from_view(current_view_));
        if (current_model_workflow == normalized_model_workflow(routed.workflow)) {
            launch_custom_model_preflight(routed.workflow, custom->artifact_kind, custom->preset_name,
                                          custom->resolution, custom->path, device_id);
        }
        refresh_browser_artifacts();
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
        if (train_.input_mode == TrainInputMode::Weights && train_.model_input == ModelInputMode::None) {
            throw std::runtime_error("browser train.start requires selected model weights");
        }
        require_custom_model_ready(mmltk::browser::Workflow::Train, train_, train_.weights_path, {}, {});
        const mmltk::browser::ArtifactState artifact_state = artifact_controller_.snapshot();
        const mmltk::browser::DatasetArtifactState& dataset_state = artifact_state.dataset;
        if (dataset_state.phase != mmltk::browser::ArtifactOperationPhase::Complete || !dataset_state.compatible) {
            throw std::runtime_error(
                "browser train.start blocked by compiled dataset: " +
                (dataset_state.error.empty() ? "compiled dataset inspection is incomplete" : dataset_state.error));
        }
        if (train_.input_mode == TrainInputMode::Weights && train_.weights_path.empty()) {
            throw std::runtime_error("browser train.start blocked by canonical weight acquisition: " +
                                     (artifact_state.weight.error.empty() ? "selected model weights are not ready"
                                                                          : artifact_state.weight.error));
        }
        if (train_.input_mode == TrainInputMode::Weights && is_managed_canonical_weight_path(train_.weights_path)) {
            const std::string expected_weight_path = canonical_weight_cache_path(train_.preset_name);
            if (!path_matches(train_.weights_path, expected_weight_path) ||
                artifact_state.weight.phase != mmltk::browser::ArtifactOperationPhase::Complete ||
                artifact_state.weight.preset_name != train_.preset_name ||
                !path_matches(artifact_state.weight.path, expected_weight_path)) {
                throw std::runtime_error(
                    "browser train.start blocked until the selected model's canonical weight checksum is verified");
            }
        }
        local_train_controller_.start(train_, train_.preset_name);
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
        if (predict_.model_input == ModelInputMode::None) {
            throw std::runtime_error("browser predict.start requires a selected model artifact");
        }
        require_custom_model_ready(routed.workflow, predict_, predict_.weights_path, predict_.onnx_path,
                                   predict_.tensorrt_path);

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
        if (validate_.model_input == ModelInputMode::None) {
            throw std::runtime_error("browser validate.start requires a selected model artifact");
        }
        require_custom_model_ready(mmltk::browser::Workflow::Validate, validate_, validate_.weights_path,
                                   validate_.onnx_path, validate_.tensorrt_path);
        launch_validate_job();
        return;
    }

    if (const auto* export_start = std::get_if<mmltk::browser::ExportStartIntent>(&routed.payload);
        export_start != nullptr) {
        apply_settings_patch(export_start->options);
        current_view_ = View::Export;
        if (export_.model_input == ModelInputMode::None) {
            throw std::runtime_error("browser export.start requires a selected model artifact");
        }
        require_custom_model_ready(mmltk::browser::Workflow::Export, export_, export_.weights_path, export_.onnx_path,
                                   {});
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
            case mmltk::browser::AnnotateSetupAction::StartLive: {
                require_custom_model_ready(mmltk::browser::Workflow::Annotate, annotate_, annotate_.weights_path,
                                           annotate_.onnx_path, annotate_.tensorrt_path);
                const std::string device_path =
                    "/dev/video" + std::to_string(std::max(0, annotate_.source.device_index));
                mmltk::logging::logger("gui")->info(
                    "browser annotate.live.start received: source={} device={} capture={}x{} fps={} buffers={} "
                    "model_input={} analyzer_attached=false live_capture_supported={} running={} block_actions={}",
                    source_kind_label(annotate_.source.kind), device_path, std::max(1, annotate_.source.capture_width),
                    std::max(1, annotate_.source.capture_height), std::max(1, annotate_.source.capture_fps),
                    std::max(1, annotate_.source.v4l2_buffer_count), model_input_log_name(annotate_.model_input),
                    live_capture_supported(), annotation_live_running(), block_actions);
                const auto fail_live_start = [this](std::string message) {
                    annotate_live_startup_state_ = AnnotateLiveStartupState::Failed;
                    annotate_live_start_error_ = std::move(message);
                    annotate_save_error_ = annotate_live_start_error_;
                    live_preview_error_ = annotate_live_start_error_;
                    log_gui_error("annotate live start error", annotate_live_start_error_);
                };
                if (!live_video) {
                    fail_live_start("browser annotate.setup start_live requires a video-stream annotate source");
                    return;
                }
                if (!live_capture_supported()) {
                    fail_live_start("browser annotate.setup start_live is not available in this build");
                    return;
                }
                if (annotation_live_running()) {
                    fail_live_start("browser annotate.setup start_live is already active");
                    return;
                }
                if (block_actions) {
                    fail_live_start(
                        "browser annotate.setup start_live is blocked while annotate shell actions are busy");
                    return;
                }
                if (const std::string live_source_error = validate_video_stream_source(annotate_.source);
                    !live_source_error.empty()) {
                    fail_live_start(live_source_error);
                    return;
                }
                annotation_workflow_coordinator_->start_live_session();
                if (!annotation_live_running()) {
                    if (annotate_live_start_error_.empty()) {
                        fail_live_start("browser annotate.setup start_live failed");
                    }
                }
                return;
            }
            case mmltk::browser::AnnotateSetupAction::StopLive:
                annotate_live_start_error_.clear();
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
        ui_settings_.annotation_brush_radius = annotate_brush_radius_;
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
        launch_browser_file_dialog(routed.workflow, *file_dialog);
        return;
    }

    if (const auto* viewport = std::get_if<mmltk::browser::ViewportCommitIntent>(&routed.payload);
        viewport != nullptr) {
        queue_browser_viewport_commit(routed.workflow, *viewport);
        return;
    }

    if (const auto* workspace_bounds = std::get_if<mmltk::browser::WorkspaceBoundsIntent>(&routed.payload);
        workspace_bounds != nullptr) {
        browser_workspace_bounds_state_ = BrowserWorkspaceBoundsState{
            routed.workflow,
            *workspace_bounds,
        };
        return;
    }
}

void App::drain_background_work() {
    ui_callbacks_.drain(kMaxUiCallbacksPerPoll);
    local_train_controller_.poll();
    remote_train_controller_.poll(vast_api_key_);
    live_predict_controller_.poll_status();
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (static_workspace_compositor_ != nullptr && static_workspace_compositor_->running()) {
        try {
            (void)static_workspace_compositor_->refresh();
        } catch (const std::exception& error) {
            live_preview_error_ = error.what();
        }
    }
#endif
    refresh_browser_artifacts();

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
        refresh_annotate_live_startup_state(*live_session_status_);
        if (now >= impl_->next_live_pipeline_status_log_) {
            const frameshow::CaptureStats ingress_stats = live_controller_->ingress().snapshot_stats();
            mmltk::logging::logger("gui")->info(
                "live pipeline progress: ingress queued={} dequeued={} inference_published={} "
                "preview_published={} dropped={} inference_backpressure={} preview_backpressure={} "
                "requeue_failures={} fanout_release_backlog={} acquire_misses={} compositor_frames={} "
                "compositor_last_frame={} last_stage={} last_stage_frame={} "
                "source_acquire_waits={} source_leases_acquired={} source_leases_released={} "
                "source_stale_releases={} source_slot_pressure={} source_release_latency_ns={}",
                ingress_stats.queued_v4l2_buffers, ingress_stats.dequeued_v4l2_buffers,
                ingress_stats.inference_frames_published, ingress_stats.preview_frames_published,
                ingress_stats.frames_dropped, ingress_stats.inference_backpressure_drops,
                ingress_stats.preview_backpressure_drops, ingress_stats.requeue_failures,
                live_session_status_->release_backlog, live_session_status_->acquire_misses,
                live_session_status_->compositor.frames_composited,
                live_session_status_->compositor.last_frame_id.sequence, live_session_status_->last_stage_name,
                live_session_status_->last_stage_frame_id.sequence,
                live_session_status_->compositor.source_acquire_waits,
                live_session_status_->compositor.source_leases_acquired,
                live_session_status_->compositor.source_leases_released,
                live_session_status_->compositor.source_stale_releases,
                live_session_status_->compositor.source_slot_pressure,
                live_session_status_->compositor.source_release_latency_ns);
            impl_->next_live_pipeline_status_log_ = now + std::chrono::seconds(1);
        }
    } else {
        impl_->next_live_pipeline_status_log_ = {};
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
    const std::string preset_name = state.preset_name;
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
            rfdetr::export_weights_to_onnx(export_request, export_request.output_path, export_request.device_id,
                                           export_request.opset_version, export_request.simplify);
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

void App::launch_custom_model_preflight(const mmltk::browser::Workflow workflow,
                                        const mmltk::browser::CustomModelArtifactKind artifact_kind,
                                        std::string preset_name, const std::uint32_t resolution, std::string path,
                                        const int device_id) {
    model_preflight_cancel_->store(true, std::memory_order_relaxed);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    model_preflight_cancel_ = cancelled;
    const std::uint64_t generation = model_preflight_state_.generation + 1U;
    model_preflight_state_ = mmltk::browser::ModelPreflightState{
        .generation = generation,
        .workflow = normalized_model_workflow(workflow),
        .status = mmltk::browser::ModelPreflightStatus::Verifying,
        .artifact_kind = artifact_kind,
        .preset_name = preset_name,
        .resolution = resolution,
        .path = path,
        .error = {},
    };
    const std::shared_ptr<std::mutex> serialization = model_preflight_serialization_;
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [artifact_kind, path = std::filesystem::path(std::move(path)), preset_name = std::move(preset_name), resolution,
         device_id, cancelled, serialization] {
            std::lock_guard serialize(*serialization);
            require_preflight_active(*cancelled);
            try {
                run_custom_model_preflight(artifact_kind, path, preset_name, static_cast<int>(resolution), device_id,
                                           *cancelled);
                return ModelPreflightTaskResult{true, {}};
            } catch (const ModelPreflightIncompatible& error) {
                return ModelPreflightTaskResult{false, error.what()};
            }
        },
        [this, generation, cancelled](ModelPreflightTaskResult result) {
            if (cancelled->load(std::memory_order_relaxed) || model_preflight_state_.generation != generation) {
                return;
            }
            model_preflight_state_.status = result.compatible ? mmltk::browser::ModelPreflightStatus::Ready
                                                              : mmltk::browser::ModelPreflightStatus::Incompatible;
            model_preflight_state_.error = std::move(result.error);
        },
        [this, generation, cancelled](const std::string& error) {
            if (cancelled->load(std::memory_order_relaxed) || model_preflight_state_.generation != generation) {
                return;
            }
            model_preflight_state_.status = mmltk::browser::ModelPreflightStatus::Failed;
            model_preflight_state_.error = error;
        });
}

void App::launch_browser_file_dialog(const mmltk::browser::Workflow workflow,
                                     const mmltk::browser::FileDialogRequestIntent& intent) {
    if (active_file_browse_field_ != FileBrowseField::None || shutting_down_) {
        throw std::runtime_error(
            "browser file dialog request blocked while "
            "another file dialog is active");
    }
    if (intent.token == 0U) {
        throw std::runtime_error("browser file dialog request requires a nonzero token");
    }

    const mmltk::browser::BrowserFileDialogBinding binding =
        mmltk::browser::file_dialog_binding_from_id(intent.dialog_id);
    if (binding == mmltk::browser::BrowserFileDialogBinding::Unknown) {
        throw std::runtime_error("unsupported browser file dialog binding: " + intent.dialog_id);
    }
    const mmltk::browser::BrowserNativeFileDialogContractSpec* spec = browser_dialog_spec(intent.dialog_id);
    if (spec == nullptr || spec->binding != binding || spec->mode != intent.mode ||
        spec->field != intent.target_field ||
        normalized_model_workflow(spec->workflow) != normalized_model_workflow(workflow)) {
        throw std::runtime_error("browser file dialog request does not match its native contract: " + intent.dialog_id);
    }
    const std::optional<mmltk::browser::CustomModelArtifactKind> model_kind = custom_model_kind_for_binding(binding);
    if (model_kind.has_value() && (!intent.defer_apply || !custom_model_kind_allowed(workflow, *model_kind))) {
        throw std::runtime_error("custom model dialogs require a deferred, workflow-compatible selection");
    }

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
    const std::string dialog_title = intent.title.empty() ? intent.dialog_id : intent.title;
    active_browser_dialog_id_ = intent.dialog_id;
    active_browser_dialog_title_ = dialog_title;
    model_dialog_authorization_.reset();
    if (model_kind.has_value()) {
        model_dialog_authorization_ = ModelDialogAuthorization{
            .token = intent.token,
            .workflow = normalized_model_workflow(workflow),
            .artifact_kind = *model_kind,
            .dialog_id = intent.dialog_id,
            .defer_apply = intent.defer_apply,
            .consumed = false,
        };
    }
    browser_file_dialog_state_ = mmltk::browser::FileDialogState{
        .token = intent.token,
        .status = mmltk::browser::FileDialogResultStatus::Pending,
        .dialog_id = intent.dialog_id,
        .path = {},
        .error = {},
    };
    mmltk::runtime::submit_background_task(
        background_executor_, ui_callbacks_,
        [dialog_title, mode = file_picker_mode(), filters = std::move(filters)]() {
            return pick_path_with_dialog(dialog_title.c_str(), mode, filters);
        },
        [this, binding, dialog_id = intent.dialog_id, token = intent.token,
         defer_apply = intent.defer_apply](std::optional<std::string> picked) {
            if (picked.has_value()) {
                const std::string selected_path = *picked;
                if (!defer_apply) {
                    mmltk::browser::apply_browser_file_dialog_selection(
                        binding, std::move(*picked),
                        mmltk::browser::BrowserFileDialogStateAccess{&train_, &predict_, &annotate_, &validate_,
                                                                     &export_});
                    if (binding == mmltk::browser::BrowserFileDialogBinding::TrainCompiledDatasetDir) {
                        train_.use_compiled_directory_defaults = true;
                        artifact_compiled_directory_.clear();
                    } else if (binding == mmltk::browser::BrowserFileDialogBinding::TrainTrainCompiledPath ||
                               binding == mmltk::browser::BrowserFileDialogBinding::TrainValCompiledPath ||
                               binding == mmltk::browser::BrowserFileDialogBinding::TrainTestCompiledPath) {
                        train_.use_compiled_directory_defaults = false;
                    }
                    refresh_browser_artifacts();
                    if (binding == mmltk::browser::BrowserFileDialogBinding::AnnotateSingleImage ||
                        binding == mmltk::browser::BrowserFileDialogBinding::AnnotateImageFolder) {
                        annotation_workflow_coordinator_->prepare_source();
                    }
                }
                browser_file_dialog_state_ = mmltk::browser::FileDialogState{
                    .token = token,
                    .status = mmltk::browser::FileDialogResultStatus::Selected,
                    .dialog_id = dialog_id,
                    .path = selected_path,
                    .error = {},
                };
            } else {
                browser_file_dialog_state_ = mmltk::browser::FileDialogState{
                    .token = token,
                    .status = mmltk::browser::FileDialogResultStatus::Cancelled,
                    .dialog_id = dialog_id,
                    .path = {},
                    .error = {},
                };
            }
            picker_error_.clear();
            if (active_file_browse_field_ == FileBrowseField::BrowserDialog) {
                active_file_browse_field_ = FileBrowseField::None;
            }
            active_browser_dialog_id_.clear();
            active_browser_dialog_title_.clear();
        },
        [this, dialog_id = intent.dialog_id, token = intent.token](const std::string& error) {
            picker_error_ = error;
            browser_file_dialog_state_ = mmltk::browser::FileDialogState{
                .token = token,
                .status = mmltk::browser::FileDialogResultStatus::Failed,
                .dialog_id = dialog_id,
                .path = {},
                .error = error,
            };
            log_gui_error("browser file dialog error", std::format("{}: {}", dialog_id, error));
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
        [dialog = std::string(dialog_title)]() { return pick_file_with_dialog(dialog.c_str()); },
        [this, field, target](std::optional<std::string> picked) {
            if (picked.has_value()) {
                *target = std::move(*picked);
                annotation_workflow_coordinator_->prepare_source();
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
    if (live_running()) {
        (void)publish_live_manual_overlay();
        return;
    }
#if MMLTK_RFDETR_LIVE_CAPTURE
    const AnnotationFrame* frame = annotation_frame_ptr(app_.annotate_frame_);
    if (frame != nullptr && app_.static_workspace_compositor_ != nullptr) {
        const std::shared_ptr<const AnnotationProjectedScene> projected_scene = resolve_annotation_projected_scene(
            app_.annotate_workflow_, app_.annotate_document_, app_.annotate_session_, *frame);
        mmltk::live::ManualOverlayDocumentSnapshot overlay =
            projected_scene != nullptr
                ? AnnotationRenderer::build_manual_overlay_snapshot(
                      *frame, app_.annotate_document_, app_.annotate_session_, *projected_scene, crop_overlay_box())
                : AnnotationRenderer::build_manual_overlay_snapshot(*frame, app_.annotate_document_,
                                                                    app_.annotate_session_, crop_overlay_box());
        app_.static_workspace_compositor_->overlay_document().publish_snapshot(std::move(overlay));
        (void)app_.static_workspace_compositor_->refresh();
    }
#endif
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
    app_.annotate_prepare_running_ = false;
    app_.annotate_frame_load_running_ = false;
    app_.annotate_assist_summary_.clear();
    app_.annotate_assist_error_.clear();
    app_.annotate_save_summary_.clear();
    app_.annotate_save_error_.clear();
    reset_instances();
    reset_live_session_state(false, App::ActiveLiveMode::None);
#if MMLTK_RFDETR_LIVE_CAPTURE
    app_.static_workspace_start_failed_ = false;
    if (app_.static_workspace_compositor_ != nullptr) {
        app_.static_workspace_compositor_->stop();
        app_.static_workspace_compositor_.reset();
        app_.static_workspace_pool_.reset();
    }
#endif
    if (app_.annotate_.source.kind == SourceKind::VideoStream) {
        return;
    }

    if (!annotation_source_selected(app_.annotate_.source)) {
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
    if (app_.live_controller_) {
        app_.live_controller_->stop();
        app_.live_controller_.reset();
    }
    app_.live_session_status_.reset();
    app_.annotation_live_session_running_for_testing_ = false;
    app_.annotate_live_startup_state_ = AnnotateLiveStartupState::Idle;
    app_.annotate_live_startup_deadline_ = {};
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
    app_.annotate_frame_ = std::move(frame);
    const AnnotationFrame* annotation_frame = optional_value_ptr(app_.annotate_frame_);
#if MMLTK_RFDETR_LIVE_CAPTURE
    app_.static_workspace_start_failed_ = false;
    ensure_static_workspace();
#endif
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

void AnnotationWorkflowCoordinator::ensure_static_workspace() noexcept {
#if MMLTK_RFDETR_LIVE_CAPTURE
    const AnnotationFrame* frame = annotation_frame_ptr(app_.annotate_frame_);
    if (app_.annotate_.source.kind == SourceKind::VideoStream || frame == nullptr || frame->pixels_bgr.empty() ||
        app_.static_workspace_compositor_ != nullptr || app_.static_workspace_start_failed_ ||
        !mmltk::live::workspace_vulkan_adapter_ready()) {
        return;
    }
    try {
        app_.static_workspace_pool_ =
            std::make_shared<mmltk::live::WorkspaceSurfacePool>(mmltk::live::WorkspaceSurfacePoolConfig{
                app_.annotate_.device_id, frame->width, frame->height, 4U, 1U,
                mmltk::live::require_workspace_drm_modifier(app_.annotate_.device_id)});
        app_.static_workspace_compositor_ = std::make_unique<mmltk::live::StaticWorkspaceCompositor>(
            app_.static_workspace_pool_, app_.annotate_.device_id, frame->width, frame->height, 2U,
            annotation_frame_capture_width(*frame), annotation_frame_capture_height(*frame));
        app_.static_workspace_compositor_->start();
        app_.static_workspace_compositor_->set_source_bgr(
            frame->pixels_bgr.data(), frame->pixels_bgr.size(), frame->width, frame->height,
            mmltk::live::LiveFrameId{app_.static_workspace_pool_->descriptor().generation, frame->frame_id},
            frame->source_name,
            mmltk::live::LiveCaptureRegion{frame->view_x, frame->view_y, frame->width, frame->height});
        invalidate_preview();
    } catch (const std::exception& error) {
        app_.static_workspace_start_failed_ = true;
        app_.static_workspace_compositor_.reset();
        app_.static_workspace_pool_.reset();
        app_.annotate_save_error_ = error.what();
        log_gui_error("static annotation workspace error", app_.annotate_save_error_);
    }
#endif
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
            app_.annotate_categories_, !frame_ptr->pixels_bgr.empty(), crop_overlay_box(),
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
        [this, generation, projected_scene_snapshot](const AnnotationWorkflowPreparedPreview& prepared) mutable {
            end_annotation_workflow_preview(app_.annotate_workflow_);
            const AnnotationFrame* annotation_frame = optional_value_ptr(app_.annotate_frame_);
            if (annotation_workflow_preview_generation_matches(app_.annotate_workflow_, generation) &&
                annotation_frame != nullptr && annotation_frame->frame_id == prepared.frame_id) {
                app_.annotate_resolved_instances_ = prepared.resolved_objects;
                const bool preview_ready = !annotation_frame->pixels_bgr.empty();
                if (preview_ready) {
                    app_.live_preview_error_.clear();
                    note_annotation_workflow_preview_ready(app_.annotate_workflow_, *annotation_frame);
                }
#if MMLTK_RFDETR_LIVE_CAPTURE
                if (app_.static_workspace_compositor_ != nullptr) {
                    mmltk::live::ManualOverlayDocumentSnapshot overlay =
                        projected_scene_snapshot != nullptr
                            ? AnnotationRenderer::build_manual_overlay_snapshot(
                                  *annotation_frame, app_.annotate_document_, app_.annotate_session_,
                                  *projected_scene_snapshot, crop_overlay_box())
                            : AnnotationRenderer::build_manual_overlay_snapshot(
                                  *annotation_frame, app_.annotate_document_, app_.annotate_session_,
                                  crop_overlay_box());
                    app_.static_workspace_compositor_->overlay_document().publish_snapshot(std::move(overlay));
                    (void)app_.static_workspace_compositor_->refresh();
                }
#endif
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

bool AnnotationWorkflowCoordinator::publish_live_manual_overlay() {
    if (!live_running() || app_.live_controller_ == nullptr) {
        return false;
    }
    const AnnotationFrame* frame_ptr = annotation_frame_ptr(app_.annotate_frame_);
    if (frame_ptr == nullptr) {
        return false;
    }
    return publish_annotation_workflow_live_manual_overlay(
        app_.annotate_workflow_, app_.annotate_document_, app_.annotate_session_, *frame_ptr, crop_overlay_box(),
        [this](mmltk::live::ManualOverlayDocumentSnapshot snapshot) {
            app_.live_controller_->manual_overlay_document().publish_snapshot(std::move(snapshot));
        });
}

bool AnnotationWorkflowCoordinator::live_running() const {
    return app_.active_live_mode_ == App::ActiveLiveMode::Annotate &&
           (app_.live_controller_ != nullptr || app_.annotation_live_session_running_for_testing_);
}

std::optional<AnnotationBox> AnnotationWorkflowCoordinator::crop_overlay_box() const {
    if (app_.annotate_.source.kind != SourceKind::VideoStream || !app_.annotate_.full_frame) {
        return std::nullopt;
    }
    return resolved_video_crop_box(app_.annotate_.source);
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
    ensure_static_workspace();
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
        (void)publish_live_manual_overlay();
    }

    if (annotation_workflow_preview_pending(app_.annotate_workflow_, annotation_frame_ptr(app_.annotate_frame_))) {
        submit_preview();
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
    app_.annotate_live_start_error_.clear();
    app_.live_preview_error_.clear();
    try {
        if (app_.annotate_.source.kind != SourceKind::VideoStream) {
            throw std::runtime_error("annotation live capture requires a video source");
        }

        const std::string device_path = "/dev/video" + std::to_string(std::max(0, app_.annotate_.source.device_index));
        mmltk::live::LiveSessionConfig session_config = build_annotation_live_session_config(app_.annotate_);
        mmltk::logging::logger("gui")->info(
            "annotate live capture starting: device={} capture={}x{} fps={} v4l2_buffers={} "
            "preview_buffers={} output_slots={} cuda_device={} model_input={} analyzer_attached=false "
            "live_capture_supported={}",
            session_config.capture.device_path, session_config.capture.width, session_config.capture.height,
            session_config.capture.fps, session_config.capture.v4l2_buffer_count,
            session_config.capture.preview_buffer_count, session_config.output_slot_count,
            session_config.cuda_device_index, model_input_log_name(app_.annotate_.model_input),
            live_capture_supported());

        if (app_.impl_->annotation_live_session_start_override_) {
            app_.impl_->annotation_live_session_start_override_(session_config);
            auto status = std::make_unique<mmltk::live::LiveSessionStatus>();
            status->running = true;
            app_.live_session_status_ = std::move(status);
            app_.annotation_live_session_running_for_testing_ = true;
            app_.active_live_mode_ = App::ActiveLiveMode::Annotate;
            app_.annotate_live_startup_state_ = AnnotateLiveStartupState::Running;
            mmltk::logging::logger("gui")->info("annotate live capture started: device={}", device_path);
            return;
        }

        auto controller = std::make_shared<mmltk::live::LiveSessionController>(std::move(session_config));
        app_.live_controller_ = std::move(controller);
        app_.active_live_mode_ = App::ActiveLiveMode::Annotate;
        app_.annotate_live_startup_state_ = AnnotateLiveStartupState::Starting;
        app_.annotate_live_startup_deadline_ = std::chrono::steady_clock::now() + kAnnotateLiveStartupDeadline;
        app_.live_controller_->start();
        app_.live_controller_->manual_overlay_document().clear(
            static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_width)),
            static_cast<std::uint32_t>(std::max(1, app_.annotate_.source.capture_height)));
        app_.live_session_status_ =
            std::make_unique<mmltk::live::LiveSessionStatus>(app_.live_controller_->snapshot_status());
        app_.refresh_annotate_live_startup_state(*app_.live_session_status_);
        mmltk::logging::logger("gui")->info("annotate live capture started: device={}", device_path);
    } catch (const std::exception& error) {
        if (app_.live_controller_) {
            try {
                app_.live_controller_->stop();
            } catch (const std::exception& stop_error) {
                log_gui_error("annotate live stop after start failure", stop_error.what());
            }
            app_.live_controller_.reset();
        }
        app_.active_live_mode_ = App::ActiveLiveMode::None;
        app_.live_session_status_.reset();
        app_.annotation_live_session_running_for_testing_ = false;
        app_.annotate_live_startup_state_ = AnnotateLiveStartupState::Failed;
        app_.annotate_save_error_ = error.what();
        app_.annotate_live_start_error_ = error.what();
        app_.live_preview_error_ = error.what();
        log_gui_error("annotate live start error", app_.annotate_live_start_error_);
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

void App::set_annotation_live_session_start_override_for_testing(
    std::function<void(const mmltk::live::LiveSessionConfig&)> callback) {
    impl_->annotation_live_session_start_override_ = std::move(callback);
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

    const mmltk::live::WorkspacePresentSnapshot present = live_controller_->latest_workspace_present();
    if (!present.valid) {
        return std::nullopt;
    }

    const mmltk::live::LiveCaptureRegion region =
        preview_region_for_source(present.source_region, annotate_.source, annotate_.full_frame);
    const mmltk::live::LiveCaptureRegion displayed_region{
        region.x,
        region.y,
        region.width == 0U ? present.dims.width : region.width,
        region.height == 0U ? present.dims.height : region.height,
    };
    const AnnotationFrame* current_annotation_frame = optional_value_ptr(annotate_frame_);
    if (current_annotation_frame != nullptr && current_annotation_frame->live_frame_id == present.frame_id &&
        current_annotation_frame->view_x == displayed_region.x &&
        current_annotation_frame->view_y == displayed_region.y &&
        current_annotation_frame->width == displayed_region.width &&
        current_annotation_frame->height == displayed_region.height) {
        return std::nullopt;
    }

    AnnotationFrame frame;
    frame.source_name = "live";
    frame.source_path = "/dev/video" + std::to_string(std::max(0, annotate_.source.device_index));
    frame.frame_id = present.frame_id.sequence;
    frame.live_frame_id = present.frame_id;
    frame.width = displayed_region.width;
    frame.height = displayed_region.height;
    frame.view_x = displayed_region.x;
    frame.view_y = displayed_region.y;
    frame.capture_width = static_cast<std::uint32_t>(std::max(1, annotate_.source.capture_width));
    frame.capture_height = static_cast<std::uint32_t>(std::max(1, annotate_.source.capture_height));
    return frame;
}

bool App::has_static_preview() const {
    return !live_predict_running() && !live_static_preview_source_name_.empty();
}

void App::clear_static_preview() {
    if (live_predict_running()) {
        return;
    }
    live_static_preview_source_name_.clear();
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (static_workspace_compositor_ != nullptr) {
        static_workspace_compositor_->stop();
        static_workspace_compositor_.reset();
        static_workspace_pool_.reset();
    }
#endif
    live_predict_controller_.clear_errors();
    live_preview_error_.clear();
}

void App::begin_live_predict_preview_stream(const int device_id) {
    (void)device_id;
    active_live_mode_ = ActiveLiveMode::Predict;
}

void App::end_live_predict_preview_stream() {
    active_live_mode_ = ActiveLiveMode::None;
}

void App::reset_live_predict_preview_state(const bool clear_frame) {
    (void)clear_frame;
    live_static_preview_source_name_.clear();
    live_preview_error_.clear();
    live_crop_overlay_mode_ = false;
    live_crop_drag_session_ = {};
}

void App::refresh_annotate_live_startup_state(const mmltk::live::LiveSessionStatus& status) {
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (active_live_mode_ != ActiveLiveMode::Annotate || live_controller_ == nullptr ||
        annotate_live_startup_state_ != AnnotateLiveStartupState::Starting) {
        return;
    }
    if (!status.last_error.empty()) {
        annotate_live_startup_state_ = AnnotateLiveStartupState::Failed;
        annotate_live_start_error_ = status.last_error;
        live_preview_error_ = status.last_error;
        log_gui_error("annotate live start error", annotate_live_start_error_);
        return;
    }
    if (status.running) {
        annotate_live_startup_state_ = AnnotateLiveStartupState::Running;
        annotate_live_start_error_.clear();
        return;
    }
    if (std::chrono::steady_clock::now() < annotate_live_startup_deadline_) {
        return;
    }
    annotate_live_startup_state_ = AnnotateLiveStartupState::Failed;
    annotate_live_start_error_ = "annotate live capture did not enter the running state before the startup deadline";
    live_preview_error_ = annotate_live_start_error_;
    log_gui_error("annotate live start error", annotate_live_start_error_);
#else
    (void)status;
#endif
}

void App::apply_static_preview(StillImagePreview preview) {
    if (live_predict_running()) {
        throw std::runtime_error("cannot apply a static preview while live predict is running");
    }

    live_static_preview_source_name_ = preview.source_name;
    if (preview.width == 0U || preview.height == 0U) {
        live_static_preview_source_name_.clear();
        return;
    }
#if MMLTK_RFDETR_LIVE_CAPTURE
    if (preview.pixels_bgr.size() != static_cast<std::size_t>(preview.width) * preview.height * 3U) {
        throw std::runtime_error("static prediction preview source pixels do not match its dimensions");
    }
    if (static_workspace_compositor_ != nullptr) {
        static_workspace_compositor_->stop();
    }
    static_workspace_pool_ = std::make_shared<mmltk::live::WorkspaceSurfacePool>(
        mmltk::live::WorkspaceSurfacePoolConfig{predict_.device_id, preview.width, preview.height, 4U, 1U,
                                                mmltk::live::require_workspace_drm_modifier(predict_.device_id)});
    static_workspace_compositor_ = std::make_unique<mmltk::live::StaticWorkspaceCompositor>(
        static_workspace_pool_, predict_.device_id, preview.width, preview.height);
    static_workspace_compositor_->start();
    static_workspace_compositor_->set_source_bgr(
        preview.pixels_bgr.data(), preview.pixels_bgr.size(), preview.width, preview.height,
        mmltk::live::LiveFrameId{static_workspace_pool_->descriptor().generation, 1U}, preview.source_name);
    static_workspace_compositor_->overlay_document().publish_snapshot(std::move(preview.prediction_overlay));
    (void)static_workspace_compositor_->refresh();
#else
    (void)preview;
#endif
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
        background_executor_, ui_callbacks_, predict_, predict_.preset_name,
        [this](const int device_id) {
            begin_live_predict_preview_stream(device_id);
            current_view_ = View::Live;
        },
        [this](const std::string& error) {
            live_preview_error_ = error;
            log_gui_error("live start error", error);
        });
}

void App::stop_live_predict_session() {
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

void App::apply_workflow_preset_defaults(const View workflow_view, const std::string_view preset_name) {
    std::string resolved_preset_name(preset_name);
    const auto& preset = resolve_model_preset(resolved_preset_name);
    WorkflowModelSelectionState& selection =
        workflow_model_selection(workflow_view, train_, validate_, predict_, annotate_, export_);
    const mmltk::browser::WeightArtifactState weight = artifact_controller_.snapshot().weight;
    if (workflow_view == current_view_ && selection.model_source == ModelSelectionSource::Canonical &&
        selection.preset_name == resolved_preset_name && weight.preset_name == resolved_preset_name &&
        (weight.phase == mmltk::browser::ArtifactOperationPhase::Failed ||
         weight.phase == mmltk::browser::ArtifactOperationPhase::Cancelled)) {
        artifact_weight_preset_.clear();
    }
    switch (workflow_view) {
        case View::Train: {
            train_.weights_path.clear();
            train_.input_mode = TrainInputMode::Weights;
            break;
        }
        case View::Validate:
            validate_.weights_path.clear();
            validate_.onnx_path.clear();
            validate_.tensorrt_path.clear();
            break;
        case View::Predict:
        case View::Live:
            predict_.weights_path.clear();
            predict_.onnx_path.clear();
            predict_.tensorrt_path.clear();
            break;
        case View::Annotate:
            annotate_.weights_path.clear();
            annotate_.onnx_path.clear();
            annotate_.tensorrt_path.clear();
            break;
        case View::Export:
            export_.weights_path.clear();
            export_.onnx_path.clear();
            break;
    }
    selection.preset_name = resolved_preset_name;
    selection.model_resolution = preset.resolution;
    selection.model_source = ModelSelectionSource::Canonical;
    selection.model_input = ModelInputMode::Weights;
    switch (workflow_view) {
        case View::Train:
            train_.eval_max_dets = preset.num_select;
            apply_train_recipe(train_, current_train_recipe(resolved_preset_name, train_.optimizer),
                               train_.recipe_overrides);
            break;
        case View::Validate:
            validate_.resolution = preset.resolution;
            validate_.eval_max_dets = preset.num_select;
            break;
        case View::Predict:
        case View::Live:
            predict_.max_dets_per_image = preset.num_select;
            break;
        case View::Annotate:
            annotate_.max_dets_per_image = preset.num_select;
            break;
        case View::Export:
            break;
    }
}

void App::refresh_browser_artifacts() {
    if (train_.compiled_dataset_dir != artifact_compiled_directory_) {
        artifact_compiled_directory_ = train_.compiled_dataset_dir;
        if (train_.use_compiled_directory_defaults) {
            infer_compiled_dataset_paths();
        }
    }

    const mmltk::browser::ArtifactState artifact_state = artifact_controller_.snapshot();
    std::string dataset_signature;
    const std::string resolution_signature = std::to_string(train_.model_resolution);
    dataset_signature.reserve(train_.preset_name.size() + resolution_signature.size() +
                              train_.train_compiled_path.size() + train_.val_compiled_path.size() +
                              train_.test_compiled_path.size() + 5U);
    for (const std::string_view part :
         {std::string_view{train_.preset_name}, std::string_view{resolution_signature},
          std::string_view{train_.train_compiled_path}, std::string_view{train_.val_compiled_path},
          std::string_view{train_.test_compiled_path}}) {
        dataset_signature.append(part);
        dataset_signature.push_back('\0');
    }
    if (dataset_signature != artifact_dataset_signature_) {
        artifact_dataset_signature_ = std::move(dataset_signature);
        artifact_controller_.inspect_dataset(train_.train_compiled_path, train_.val_compiled_path,
                                             train_.test_compiled_path, train_.preset_name,
                                             static_cast<std::uint32_t>(std::max(1, train_.model_resolution)));
    }

    WorkflowModelSelectionState& active_selection =
        workflow_model_selection(current_view_, train_, validate_, predict_, annotate_, export_);
    std::string* active_weights_path = nullptr;
    std::string* active_onnx_path = nullptr;
    std::string* active_tensorrt_path = nullptr;
    int active_device_id = 0;
    switch (current_view_) {
        case View::Train:
            active_weights_path = &train_.weights_path;
            active_device_id = train_.local_device_ids.empty() ? 0 : train_.local_device_ids.front();
            break;
        case View::Validate:
            active_weights_path = &validate_.weights_path;
            active_onnx_path = &validate_.onnx_path;
            active_tensorrt_path = &validate_.tensorrt_path;
            active_device_id = validate_.device_id;
            break;
        case View::Predict:
        case View::Live:
            active_weights_path = &predict_.weights_path;
            active_onnx_path = &predict_.onnx_path;
            active_tensorrt_path = &predict_.tensorrt_path;
            active_device_id = predict_.device_id;
            break;
        case View::Annotate:
            active_weights_path = &annotate_.weights_path;
            active_onnx_path = &annotate_.onnx_path;
            active_tensorrt_path = &annotate_.tensorrt_path;
            active_device_id = annotate_.device_id;
            break;
        case View::Export:
            active_weights_path = &export_.weights_path;
            active_onnx_path = &export_.onnx_path;
            active_device_id = export_.device_id;
            break;
    }

    const mmltk::browser::Workflow active_workflow =
        normalized_model_workflow(mmltk::browser::workflow_from_view(current_view_));
    if (active_selection.model_input == ModelInputMode::None) {
        model_preflight_cancel_->store(true, std::memory_order_relaxed);
        model_preflight_state_ = {};
        if (active_weights_path != nullptr) {
            active_weights_path->clear();
        }
        if (active_onnx_path != nullptr) {
            active_onnx_path->clear();
        }
        if (active_tensorrt_path != nullptr) {
            active_tensorrt_path->clear();
        }
        if (!artifact_weight_preset_.empty()) {
            artifact_controller_.cancel_weight_acquisition();
            artifact_weight_preset_.clear();
        }
        auto_weight_path_.clear();
        return;
    }
    if (active_selection.model_source == ModelSelectionSource::Canonical) {
        model_preflight_cancel_->store(true, std::memory_order_relaxed);
        model_preflight_state_ = {};
        const mmltk::browser::WeightArtifactState& weight_state = artifact_state.weight;
        const std::string expected_weight_path = canonical_weight_cache_path(active_selection.preset_name);
        const bool managed_weight_ready = weight_state.phase == mmltk::browser::ArtifactOperationPhase::Complete &&
                                          weight_state.status == mmltk::browser::WeightArtifactStatus::Ready &&
                                          weight_state.preset_name == active_selection.preset_name &&
                                          path_matches(weight_state.path, expected_weight_path);
        if (active_weights_path != nullptr && is_managed_canonical_weight_path(*active_weights_path) &&
            (!path_matches(*active_weights_path, expected_weight_path) || !managed_weight_ready)) {
            active_weights_path->clear();
        }
        if (managed_weight_ready && active_weights_path != nullptr && active_weights_path->empty()) {
            *active_weights_path = weight_state.path;
            if (current_view_ == View::Train) {
                auto_weight_path_ = weight_state.path;
            }
        }
        if (active_selection.preset_name != artifact_weight_preset_) {
            if (active_weights_path != nullptr && is_managed_canonical_weight_path(*active_weights_path)) {
                active_weights_path->clear();
            }
            auto_weight_path_.clear();
            artifact_weight_preset_ = active_selection.preset_name;
            const std::string requested_preset = active_selection.preset_name;
            const View requested_view = current_view_ == View::Live ? View::Predict : current_view_;
            if (impl_->intent_trace_enabled_.load(std::memory_order_acquire)) {
                impl_->write_artifact_trace("weight.acquire_dispatched",
                                            {{"preset_name", requested_preset},
                                             {"workflow", std::string(mmltk::browser::workflow_name(active_workflow))},
                                             {"previous_status", static_cast<int>(weight_state.status)},
                                             {"previous_phase", static_cast<int>(weight_state.phase)}});
            }
            artifact_controller_.acquire_weight(
                requested_preset, [this, requested_preset, requested_view](const std::string& path) {
                    WorkflowModelSelectionState& requested_selection =
                        workflow_model_selection(requested_view, train_, validate_, predict_, annotate_, export_);
                    if (requested_selection.preset_name != requested_preset ||
                        requested_selection.model_source != ModelSelectionSource::Canonical) {
                        return;
                    }
                    switch (requested_view) {
                        case View::Train:
                            train_.weights_path = path;
                            auto_weight_path_ = path;
                            break;
                        case View::Validate:
                            validate_.weights_path = path;
                            break;
                        case View::Predict:
                        case View::Live:
                            predict_.weights_path = path;
                            break;
                        case View::Annotate:
                            annotate_.weights_path = path;
                            break;
                        case View::Export:
                            export_.weights_path = path;
                            break;
                    }
                });
        }
        return;
    }

    if (!artifact_weight_preset_.empty()) {
        artifact_controller_.cancel_weight_acquisition();
        artifact_weight_preset_.clear();
        auto_weight_path_.clear();
    }
    const std::string* custom_path = active_weights_path;
    mmltk::browser::CustomModelArtifactKind custom_kind = mmltk::browser::CustomModelArtifactKind::Weights;
    if (active_selection.model_input == ModelInputMode::Onnx) {
        custom_path = active_onnx_path;
        custom_kind = mmltk::browser::CustomModelArtifactKind::Onnx;
    } else if (active_selection.model_input == ModelInputMode::TensorRt) {
        custom_path = active_tensorrt_path;
        custom_kind = mmltk::browser::CustomModelArtifactKind::TensorRt;
    }
    if (custom_path == nullptr || custom_path->empty()) {
        model_preflight_cancel_->store(true, std::memory_order_relaxed);
        model_preflight_state_ = mmltk::browser::ModelPreflightState{
            .generation = model_preflight_state_.generation + 1U,
            .workflow = active_workflow,
            .status = mmltk::browser::ModelPreflightStatus::Failed,
            .artifact_kind = custom_kind,
            .preset_name = active_selection.preset_name,
            .resolution = static_cast<std::uint32_t>(std::max(0, active_selection.model_resolution)),
            .path = {},
            .error = "custom model selection has no artifact path",
        };
        return;
    }
    const bool preflight_matches =
        model_preflight_state_.workflow == active_workflow && model_preflight_state_.artifact_kind == custom_kind &&
        model_preflight_state_.preset_name == active_selection.preset_name &&
        model_preflight_state_.resolution == static_cast<std::uint32_t>(active_selection.model_resolution) &&
        path_matches(model_preflight_state_.path, *custom_path);
    if (!preflight_matches) {
        launch_custom_model_preflight(active_workflow, custom_kind, active_selection.preset_name,
                                      static_cast<std::uint32_t>(active_selection.model_resolution), *custom_path,
                                      active_device_id);
    }
}

void App::infer_compiled_dataset_paths() {
    if (train_.compiled_dataset_dir.empty()) {
        train_.train_compiled_path.clear();
        train_.val_compiled_path.clear();
        train_.test_compiled_path.clear();
        return;
    }
    const std::filesystem::path directory(train_.compiled_dataset_dir);
    train_.train_compiled_path = (directory / "train.bin").string();
    train_.val_compiled_path = (directory / "val.bin").string();
    const std::filesystem::path test_path = directory / "test.bin";
    train_.test_compiled_path = std::filesystem::is_regular_file(test_path) ? test_path.string() : std::string{};
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
#undef annotate_live_start_error_
#undef annotate_live_startup_state_
#undef annotate_live_startup_deadline_
#undef live_controller_
#undef live_session_status_
#undef annotation_live_session_running_for_testing_
#undef active_live_mode_
#undef ui_callbacks_
#undef background_executor_
#undef artifact_controller_
#undef live_predict_controller_
#undef vast_query_controller_
#undef remote_train_controller_
#undef local_train_controller_
#undef static_workspace_pool_
#undef static_workspace_compositor_
#undef static_workspace_start_failed_
#undef live_static_preview_source_name_
#undef live_preview_error_
#undef live_preview_fit_to_capture_
#undef live_crop_overlay_mode_
#undef shutting_down_
#undef live_crop_drag_session_
#undef active_file_browse_field_
#undef active_browser_dialog_id_
#undef active_browser_dialog_title_
#undef browser_file_dialog_state_
#undef model_dialog_authorization_
#undef model_preflight_state_
#undef model_preflight_cancel_
#undef model_preflight_serialization_
#undef picker_error_
#undef artifact_dataset_signature_
#undef artifact_compiled_directory_
#undef artifact_weight_preset_
#undef auto_weight_path_
#undef vast_api_key_
#undef model_registry_
#undef settings_persistence_
#undef annotation_workflow_coordinator_
#undef browser_workspace_viewport_state_
#undef browser_workspace_bounds_state_

}  
