#pragma once

#include "annotation/controller.h"
#include "annotation/document/document.h"
#include "annotation/document/edit.h"
#include "annotation/render/renderer.h"
#include "annotation/save_tab.h"
#include "annotation/session.h"
#include "annotation/setup_tab.h"
#include "annotation/workflow.h"
#include "annotation_core.h"
#include "canvas_layers.h"
#include "gui_settings.h"
#include "live_predict_controller.h"
#include "local_train_controller.h"
#include "model_input_ui.h"
#include "preview_rect_drag.h"
#include "still_image_preview.h"
#include "train_command.h"
#include "vast_query_controller.h"
#include "view_state.h"

#include <cstdint>

#include "mmltk/runtime/async_runtime.h"
#include "mmltk/runtime/model_registry.h"

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

namespace mmltk::live {
class LiveSessionController;
struct LiveSessionStatus;
}

namespace mmltk::gui {

class LivePreviewTexture;
class PreviewInteractionOverlaySurface;
struct AnnotationSidebarMutationResult;
struct AnnotationSidebarViewModel;
struct AnnotationMaskTabActions;
struct AnnotationObjectsTabActions;

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

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class App {
public:
    App(GLFWwindow* main_window, std::string vast_api_key, std::string settings_path);
    ~App();

    static void apply_style();

    void poll_background_work();
    void render();
    void shutdown();

private:
    enum class ActiveLiveMode : std::uint8_t {
        None = 0,
        Predict = 1,
        Annotate = 2,
    };

    enum class FileBrowseField : std::uint8_t {
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
    void draw_annotate_setup_tab(bool block_actions, bool live_video, bool can_use_video);
    void draw_annotate_save_tab(bool live_video);
    void apply_annotation_sidebar_mutation_result(const AnnotationSidebarMutationResult& result,
                                                  bool allow_assist);
    void apply_annotation_objects_tab_actions(const AnnotationSidebarViewModel& sidebar_model,
                                              const AnnotationObjectsTabActions& actions,
                                              bool assist_available);
    void apply_annotation_mask_tab_actions(const AnnotationMaskTabActions& actions);
    void draw_export_view();
    void draw_live_view();
    void draw_live_preview_panel();
    void draw_annotation_range_controls(const char* label,
                                        AnnotationColorRange& range,
                                        AnnotationColorRange& sibling_range);
    void draw_crop_overlay(float img_ox, float img_oy, float img_w, float img_h, float capture_w, float capture_h);
    AnnotationBox active_live_crop_box() const;
    void poll_annotate_work();
    void invalidate_annotation_preview();
    void cancel_annotation_canvas_interactions();
    void reset_annotation_instances();
    void sync_annotation_categories();
    void prepare_annotation_source();
    void load_annotation_current_frame();
    void step_annotation_current_frame(int step);
    void handle_annotation_setup_browse_request(
        AnnotationSetupBrowseRequest browse_request);
    void load_annotation_frame(AnnotationFrame frame);
    void submit_annotation_preview();
    void launch_live_predict_session();
    bool annotation_live_running() const;
    bool live_predict_running() const;
    bool live_predict_active() const;
    bool has_static_preview() const;
    void clear_static_preview();
    void apply_static_preview(StillImagePreview preview);
    void stop_live_predict_session();
    void append_job_output(std::string_view chunk);
    std::string snapshot_job_output();
    void apply_ui_settings_now(bool rebuild_font_texture);
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
    std::unique_ptr<LivePreviewTexture> live_preview_texture_;
    std::unique_ptr<PreviewInteractionOverlaySurface> live_interaction_overlay_;
    std::unique_ptr<PreviewInteractionOverlaySurface> annotate_interaction_overlay_;
    ImFont* compact_font_ = nullptr;
    ImFont* mono_font_ = nullptr;
    mmltk::runtime::UiCallbackQueue ui_callbacks_;
    mmltk::runtime::BackgroundExecutor background_executor_{2};
    LivePredictController live_predict_controller_{};
    VastQueryController vast_query_controller_;
    LocalTrainController local_train_controller_;
    std::string live_static_preview_source_name_;
    std::string live_preview_error_;
    bool live_preview_fit_to_capture_ = false;
    bool live_crop_overlay_mode_ = false;
    bool shutting_down_ = false;
    PreviewRectDragSession live_crop_drag_session_{};
    FileBrowseField active_file_browse_field_ = FileBrowseField::None;
    std::string picker_error_;
    std::string vast_api_key_;
    mmltk::runtime::ModelRegistry model_registry_;
    GuiSettingsPersistence settings_persistence_;
    bool ui_settings_window_open_ = false;
    bool ui_settings_apply_pending_ = false;
};

} // namespace mmltk::gui
