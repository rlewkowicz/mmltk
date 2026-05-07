#pragma once

#include "browser/host_api.h"
#include "gui/browser_retained_frame_registry.h"
#include "mmltk/live/live_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::live {
enum class LivePipelineStage : std::uint8_t;
struct LiveFrameId;
class LiveSessionController;
struct LiveSessionConfig;
struct LiveSessionStatus;
struct WorkspacePresentSnapshot;
}  // namespace mmltk::live

namespace mmltk::browser {
enum class Workflow : std::uint8_t;
struct IntentMessage;
struct StateSnapshot;
struct FileDialogRequestIntent;
struct ViewportCommitIntent;
struct WorkspaceRendererEventIntent;
}  // namespace mmltk::browser

namespace mmltk::gui {

class AnnotationWorkflowCoordinator;
class AnnotationController;
class LocalTrainController;
class LivePredictController;
class PreviewRectDragSession;
class RemoteTrainController;
class VastQueryController;
struct AnnotationCategories;
struct AnnotationFrame;
struct AnnotationDocument;
struct AnnotationResolvedObject;
struct AnnotationSession;
struct AnnotationSidebarMutationResult;
struct AnnotationWorkflowRuntime;
struct ExportViewState;
struct GuiSettingsSnapshot;
struct PredictViewState;
enum class RemoteGpuFamily : std::uint8_t;
struct StillImagePreview;
struct TrainViewState;
struct UiSettingsState;
struct ValidateViewState;
enum class View : std::uint8_t;

struct BrowserWorkspaceViewportState {
    mmltk::browser::Workflow workflow{};
    mmltk::browser::ViewportCommitIntent commit{};
};

struct BrowserWorkspaceBoundsState {
    mmltk::browser::Workflow workflow{};
    mmltk::browser::WorkspaceBoundsIntent bounds{};
};

struct BrowserWorkspaceNativePresentState {
    const mmltk::live::WorkspaceSwapchainDescriptor* swapchain_descriptor = nullptr;
    mmltk::live::WorkspacePresentSnapshot latest_present{};
    std::optional<BrowserWorkspaceBoundsState> bounds;
    std::optional<int> cuda_device_index;

    [[nodiscard]] bool ready() const noexcept {
        return swapchain_descriptor != nullptr && latest_present.valid;
    }
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class App {
   public:
    App(std::string vast_api_key, std::string settings_path);
    ~App() noexcept;

    void drain_background_work();
    void set_evented_work_wake_callback(std::function<void()> callback);
    void set_annotation_live_session_start_override_for_testing(
        std::function<void(const mmltk::live::LiveSessionConfig&)> callback);
    void shutdown();
    [[nodiscard]] mmltk::browser::StateSnapshot browser_state_snapshot();
    [[nodiscard]] std::optional<BrowserWorkspaceViewportState> browser_workspace_viewport_state() const;
    [[nodiscard]] std::optional<BrowserWorkspaceBoundsState> browser_workspace_bounds_state() const;
    [[nodiscard]] BrowserWorkspaceNativePresentState browser_workspace_native_present_state() const;
    void apply_browser_intent(const mmltk::browser::IntentMessage& intent);
    [[nodiscard]] std::optional<mmltk::browser::WorkspaceSurfaceInfo> acquire_workspace_surface(
        std::string_view surface_id) const;
    [[nodiscard]] std::optional<RetainedBrowserImportedFrameSource> acquire_workspace_surface_source(
        std::string_view surface_id, std::string_view revision) const;
    bool release_workspace_surface(std::string_view surface_id, std::string_view revision);
    void release_all_workspace_surface_publications();
    void record_workspace_surface_bridge_error(std::string error_message);
    void record_workspace_surface_pipeline_stage(mmltk::live::LivePipelineStage stage, std::string_view surface_id,
                                                 std::string_view revision, std::uint64_t latency_base_ns = 0) noexcept;

   private:
    struct JobOutcome;
    struct JobState;

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
        BrowserDialog,
        ExportWeights,
        ExportOnnx,
        ExportEngine,
        PredictSingleImage,
    };

    void apply_selected_preset_defaults();
    void launch_job(const std::string& label, std::function<JobOutcome()> fn);
    void launch_file_picker(FileBrowseField field, const char* dialog_title, std::string* target);
    [[nodiscard]] bool file_picker_busy(FileBrowseField field) const;

    void poll_annotate_work();
    void notify_evented_work_ready() const noexcept;
    [[nodiscard]] std::function<void()> live_workspace_ready_callback() const;
    [[nodiscard]] std::function<void(const mmltk::live::WorkspacePresentSnapshot&)> live_workspace_present_callback()
        const;
    void refresh_live_workspace_ready_callbacks();
    void invalidate_annotation_preview();
    void cancel_annotation_canvas_interactions();
    void reset_annotation_instances();
    void launch_live_predict_session();
    [[nodiscard]] bool annotation_live_running() const;
    [[nodiscard]] bool live_predict_running() const;
    [[nodiscard]] bool live_predict_active() const;
    [[nodiscard]] std::optional<AnnotationFrame> make_live_annotation_frame_from_preview() const;
    [[nodiscard]] bool browser_live_workspace_surface_retained() const noexcept;
    [[nodiscard]] bool has_static_preview() const;
    void clear_static_preview();
    void apply_static_preview(StillImagePreview preview);
    void launch_predict_job();
    void launch_validate_job();
    void launch_export_job();
    bool request_annotation_save_now();
    void apply_annotation_sidebar_mutation_result(const AnnotationSidebarMutationResult& result, bool allow_assist);
    void apply_workspace_renderer_event(const mmltk::browser::WorkspaceRendererEventIntent& intent,
                                        std::string_view intent_name);
    void record_workspace_renderer_pipeline_stage(mmltk::live::LivePipelineStage stage,
                                                  const mmltk::browser::WorkspaceRendererEventIntent& intent);
    void launch_browser_file_dialog(const mmltk::browser::FileDialogRequestIntent& intent);
    void queue_browser_viewport_commit(mmltk::browser::Workflow workflow,
                                       const mmltk::browser::ViewportCommitIntent& intent);
    void stop_live_predict_session();
    void begin_live_predict_preview_stream(int device_id);
    void end_live_predict_preview_stream();
    void reset_live_predict_preview_state(bool clear_frame);
    void refresh_annotate_live_startup_state(const mmltk::live::LiveSessionStatus& status);
    void mark_annotate_live_drawable(const mmltk::live::LiveFrameId& frame_id);
    void refresh_browser_live_frame_publications(mmltk::live::LiveSessionController* browser_live_controller,
                                                 std::string* browser_preview_error);
    void refresh_browser_retained_frame_publications(bool static_preview_visible);
    [[nodiscard]] mmltk::live::LiveSessionController* active_browser_live_controller() noexcept;
    void release_browser_live_frame_slots();
    void release_all_browser_frame_publications();
    void clear_browser_predict_publication();
    void clear_browser_annotate_publications();
    void publish_browser_predict_static_preview(StillImagePreview preview);
    void publish_browser_annotation_workspace_frame(const AnnotationFrame& frame);
    void append_job_output(std::string_view chunk);
    std::string snapshot_job_output();
    [[nodiscard]] std::vector<RemoteGpuFamily> selected_remote_gpu_families() const;
    [[nodiscard]] GuiSettingsSnapshot make_gui_settings_snapshot();
    struct BrowserPublicationState;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class AnnotationWorkflowCoordinator;
};

}  // namespace mmltk::gui
