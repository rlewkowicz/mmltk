#pragma once

#include "annotation_core.h"
#include "annotation_geometry.h"
#include "annotation_live_capture.h"
#include "canvas_layers.h"
#include "gui_settings.h"
#include "still_image_preview.h"
#include "train_command.h"
#include "train_process_runtime.h"
#include "vast_runtime.h"
#include "view_state.h"

#include "fastloader/runtime/async_runtime.h"
#include "fastloader/runtime/model_registry.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;
struct ImFont;

namespace fastloader::rfdetr {
class LivePredictSession;
struct LivePredictStatus;
}

namespace fastloader::gui {

class LivePreviewTexture;

struct JobOutcome {
    std::string summary;
    std::optional<StillImagePreview> preview;
};

struct JobState {
    bool running = false;
    std::string label;
    std::string last_summary;
    std::string last_error;
    std::string output_tail;
    std::mutex output_mutex;
};

struct AnnotateAssistSeed {
    std::string class_name;
    AnnotationBox box{};
    std::vector<std::uint8_t> mask;
    AnnotationMaskRegion mask_region{};
    std::uint64_t seed_frame_id = 0;
    bool has_mask = false;
};

struct AnnotateAssistOutcome {
    std::vector<AnnotateAssistSeed> seeds;
    std::string summary;
};

struct AnnotateSaveOutcome {
    AnnotationSaveResult save;
    AnnotationCategories categories;
    std::string summary;
};

struct RemoteQueryState {
    bool running = false;
    std::string last_summary;
    std::string last_error;
    std::vector<VastOfferSummary> results;
};

class App {
public:
    App(GLFWwindow* main_window, std::string vast_api_key);
    ~App();

    static void apply_style();

    void poll_background_work();
    void render();
    void shutdown();

private:
    enum class FileBrowseField : int {
        None = 0,
        TrainWeights,
        TrainResume,
        PredictWeights,
        PredictOnnx,
        PredictTensorRt,
        AnnotateWeights,
        AnnotateOnnx,
        AnnotateTensorRt,
        AnnotateSingleImage,
        ExportWeights,
        ExportOnnx,
        ExportEngine,
        PredictSingleImage,
    };

    void draw_preset_selector();
    void apply_selected_preset_defaults();
    void launch_job(const std::string& label, std::function<JobOutcome()> fn);
    void launch_file_picker(FileBrowseField field, const char* dialog_title, std::string* target);
    bool file_picker_busy(FileBrowseField field) const;
    void draw_menu_bar();
    void draw_settings_window();
    void draw_workflow_window();
    void draw_info_pane();
    void draw_train_view();
    void draw_validate_view();
    void draw_predict_view();
    void draw_annotate_workspace();
    void draw_annotate_utilities_pane();
    void draw_export_view();
    void draw_live_view();
    void draw_live_preview_panel();
    void draw_annotation_range_controls(const char* label,
                                        AnnotationColorRange& range,
                                        AnnotationColorRange& sibling_range);
    void draw_crop_overlay(float img_ox, float img_oy, float img_w, float img_h, float capture_w, float capture_h);
    void apply_live_crop_region();
    void sync_live_crop_draft();
    void clear_live_crop_draft();
    AnnotationBox active_live_crop_box() const;
    void poll_annotate_work();
    void invalidate_annotation_preview();
    void clear_annotation_geometry();
    bool ensure_annotation_geometry();
    void sync_annotation_instance_from_geometry(std::size_t index);
    void sync_all_annotation_instances_from_geometry();
    void reset_annotation_instances();
    void clear_annotation_save_queue();
    void sync_annotation_categories();
    void prepare_annotation_source();
    void load_annotation_current_frame();
    void load_annotation_frame(const AnnotationFrame& frame);
    void submit_annotation_preview();
    void start_annotation_live_session();
    void stop_annotation_live_session();
    void launch_annotation_assist();
    void launch_annotation_save(const AnnotationFrame& frame_snapshot,
                                const std::vector<AnnotationResolvedInstance>& resolved_instances_snapshot);
    void maybe_start_annotation_hold_save();
    void launch_live_predict_session();
    bool annotation_live_running() const;
    bool live_predict_running() const;
    bool live_predict_active() const;
    bool has_static_preview() const;
    void clear_static_preview();
    void apply_static_preview(StillImagePreview preview);
    void stop_live_predict_session();
    void refresh_local_gpus();
    void launch_remote_query();
    void launch_local_training();
    void request_stop_local_training(bool force);
    void append_job_output(std::string_view chunk);
    std::string snapshot_job_output();
    void apply_ui_settings_now(bool rebuild_font_texture);
    AnnotationInstance* selected_annotation_instance();
    std::vector<int> selected_local_device_ids() const;
    std::vector<RemoteGpuFamily> selected_remote_gpu_families() const;

    View current_view_ = View::Train;
    std::string selected_preset_name_;
    UiSettingsState ui_settings_{};
    TrainViewState train_;
    ValidateViewState validate_;
    PredictViewState predict_;
    AnnotateViewState annotate_;
    ExportViewState export_;
    JobState job_;
    std::vector<fastloader::rfdetr::PredictImageInput> annotate_inputs_;
    std::string annotate_source_signature_;
    std::size_t annotate_current_input_index_ = 0;
    std::optional<AnnotationFrame> annotate_frame_;
    std::vector<AnnotationInstance> annotate_instances_;
    std::vector<AnnotationResolvedInstance> annotate_resolved_instances_;
    std::optional<std::size_t> annotate_selected_instance_;
    AnnotationCategories annotate_categories_;
    bool annotate_categories_loaded_ = false;
    std::string annotate_categories_output_dir_;
    std::string annotate_pending_class_name_;
    RectDragState annotate_create_drag_state_{};
    RectLayerState annotate_canvas_layer_state_{};
    RectDragState annotate_direct_drag_state_{};
    std::unique_ptr<AnnotationGeometryDocument> annotate_geometry_;
    AnnotationGeometryToolMode annotate_geometry_tool_mode_ = AnnotationGeometryToolMode::Direct;
    int annotate_brush_radius_ = 12;
    int annotate_cleanup_radius_ = 1;
    bool annotate_painting_ = false;
    int annotate_last_paint_capture_x_ = 0;
    int annotate_last_paint_capture_y_ = 0;
    float annotate_canvas_scale_ = 0.0f;
    float annotate_canvas_pan_x_ = 0.0f;
    float annotate_canvas_pan_y_ = 0.0f;
    bool annotate_canvas_auto_fit_ = true;
    bool annotate_canvas_panning_ = false;
    bool annotate_preview_dirty_ = true;
    bool annotate_prepare_running_ = false;
    bool annotate_frame_load_running_ = false;
    bool annotate_preview_running_ = false;
    int annotate_preview_error_count_ = 0;
    std::uint64_t annotate_prepare_request_id_ = 0;
    std::uint64_t annotate_frame_request_id_ = 0;
    std::uint64_t annotate_preview_generation_ = 0;
    std::uint64_t annotate_preview_frame_id_ = 0;
    bool annotate_hold_save_ = false;
    bool annotate_hold_save_blocked_ = false;
    std::uint64_t annotate_last_saved_frame_id_ = 0;
    std::optional<AnnotationFrame> annotate_queued_save_frame_;
    std::vector<AnnotationResolvedInstance> annotate_queued_save_instances_;
    bool annotate_assist_running_ = false;
    std::string annotate_assist_summary_;
    std::string annotate_assist_error_;
    bool annotate_save_running_ = false;
    std::string annotate_save_summary_;
    std::string annotate_save_error_;
    std::unique_ptr<AnnotationLiveCaptureSession> annotate_live_session_;
    std::unique_ptr<fastloader::rfdetr::LivePredictSession> live_predict_session_;
    std::unique_ptr<fastloader::rfdetr::LivePredictStatus> live_predict_status_;
    std::unique_ptr<LivePreviewTexture> live_preview_texture_;
    ImFont* compact_font_ = nullptr;
    ImFont* mono_font_ = nullptr;
    std::string live_static_preview_source_name_;
    std::string live_predict_start_error_;
    std::string live_predict_action_error_;
    std::string live_preview_error_;
    bool live_predict_starting_ = false;
    bool live_predict_stopping_ = false;
    fastloader::runtime::TaskCancellation live_predict_start_task_;
    bool live_preview_fit_to_capture_ = false;
    bool live_crop_overlay_mode_ = false;
    bool shutting_down_ = false;
    fastloader::runtime::UiCallbackQueue ui_callbacks_;
    fastloader::runtime::BackgroundExecutor background_executor_{2};
    RectLayerState live_crop_layer_state_{};
    AnnotationBox live_crop_draft_box_{};
    bool live_crop_draft_active_ = false;
    FileBrowseField active_file_browse_field_ = FileBrowseField::None;
    std::string picker_error_;
    std::string vast_api_key_;
    std::vector<LocalGpuInfo> local_gpus_;
    std::vector<bool> local_gpu_selected_;
    std::string local_gpu_error_;
    bool local_gpu_refresh_running_ = false;
    RemoteQueryState remote_query_;
    LocalTrainSessionState train_process_;
    std::unique_ptr<LocalTrainSession> local_train_session_;
    std::optional<VastOfferSummary> armed_remote_offer_;
    fastloader::runtime::ModelRegistry model_registry_;
    GuiSettingsPersistence settings_persistence_{"gui.json"};
    bool ui_settings_window_open_ = false;
    bool ui_settings_apply_pending_ = false;
};

} // namespace fastloader::gui
