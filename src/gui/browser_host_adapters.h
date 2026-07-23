#pragma once

#include "gui/browser_file_dialog_contract.h"
#include "browser/host_api_intents.h"
#include "gui/browser_frame_slot_contract.h"
#include "gui/train_command.h"
#include "mmltk/live/live_compositor_status.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmltk::gui {

enum class View : std::uint8_t;
enum class SourceKind : std::uint8_t;
enum class AnnotationToolKind : std::uint8_t;
enum class AnnotationMaskCleanupOp : std::uint8_t;

struct GuiSettingsSnapshot;
struct TrainViewState;
struct ValidateViewState;
struct PredictViewState;
struct AnnotateViewState;
struct ExportViewState;
struct SourceSelectionState;
struct AnnotationFrame;
struct AnnotationCategories;

class AnnotationDocument;
class AnnotationSession;
class AnnotationController;

}  

namespace mmltk::live {

struct LiveCaptureRegion;
struct ManualOverlayDocumentSnapshot;
struct DetectBundle;
struct OutputBundle;
struct WorkspaceOutputBundle;

}  

namespace mmltk::browser {

struct GuiStateSnapshotInputs {
    const mmltk::gui::GuiSettingsSnapshot* gui = nullptr;
    std::uint64_t state_revision = 0;
    JobState job{};
    std::optional<SourceMetadata> source;
    std::optional<AnnotationDocumentState> annotation;
};

struct BrowserHostIntentContext {
    mmltk::gui::View* current_view = nullptr;
    mmltk::gui::GuiSettingsSnapshot* gui = nullptr;
    mmltk::gui::PredictViewState* predict = nullptr;
    mmltk::gui::AnnotateViewState* annotate = nullptr;
    mmltk::gui::AnnotationDocument* annotation_document = nullptr;
    mmltk::gui::AnnotationSession* annotation_session = nullptr;
    mmltk::gui::AnnotationController* annotation_controller = nullptr;
    const mmltk::gui::AnnotationFrame* annotation_frame = nullptr;
    mmltk::gui::AnnotationCategories* annotation_categories = nullptr;
    std::function<bool(std::string* error_message)> ensure_annotation_frame_pixels{};
};

struct BrowserAnnotateWorkspaceContext {
    mmltk::gui::AnnotationDocument* document = nullptr;
    mmltk::gui::AnnotationSession* session = nullptr;
    mmltk::gui::AnnotationController* controller = nullptr;
    const mmltk::gui::AnnotationFrame* frame = nullptr;
    mmltk::gui::AnnotationCategories* categories = nullptr;
    mmltk::gui::SourceSelectionState* source = nullptr;
    std::function<bool(std::string* error_message)> ensure_frame_pixels{};
};

struct BrowserBannerState {
    std::string kind;
    std::string message;
};

struct BrowserLivePreviewState {
    bool initialized = false;
    bool has_frame = false;
    CaptureRegion displayed_region{};
    std::uint64_t frame_id = 0;
    std::optional<mmltk::live::LiveFrameId> live_frame_id;
    bool fit_to_capture = false;
    bool crop_overlay_mode = false;
    std::string static_source_name;
    std::string error;
    bool interop_failed = false;
    std::string last_failure_reason;
    std::string last_failure_detail;
    std::string last_failure_revision;
    std::string last_failure_stage;
    std::optional<mmltk::live::LiveFrameId> last_failure_live_frame_id;
    std::optional<std::uint32_t> last_failure_cuda_device;
    std::string last_failure_result_code;
    std::string last_failure_result_detail;
};

struct BrowserLiveControllerState {
    bool present = false;
    bool running = false;
    std::string last_error;
};

struct BrowserLiveFanoutState {
    bool running = false;
    std::uint64_t frames_fanned_out = 0;
    std::uint64_t skipped_detect_publishes = 0;
    std::uint64_t skipped_output_publishes = 0;
    std::uint64_t release_backlog = 0;
    std::uint64_t acquire_misses = 0;
    std::string last_error;
};

struct BrowserLiveAnalyzerState {
    bool attached = false;
    bool model_hot = false;
    bool running = false;
    std::uint64_t frames_analyzed = 0;
    std::uint64_t frames_skipped = 0;
    double last_latency_ms = 0.0;
    std::string backend_name;
    std::string last_error;
};

struct BrowserLiveManualOverlayState {
    bool running = false;
    std::uint64_t generations_rendered = 0;
    std::uint64_t last_generation = 0;
    std::string last_error;
};

struct BrowserLiveCompositorState : mmltk::live::LiveCompositorTelemetry {
    std::optional<mmltk::live::LiveFrameId> last_frame_id;
    bool manual_overlay_active = false;
    bool analysis_overlay_active = false;
    std::string last_error;
};

struct BrowserLiveSingleBufferDiagnosticState {
    bool enabled = false;
    std::uint32_t frame_count = 0;
    std::uint64_t frame_budget_ns = 0;
    std::uint64_t drawn_frames = 0;
    std::uint32_t consecutive_miss_limit = 1;
    bool completed = false;
    bool analyzer_disabled = false;
    bool failed = false;
    std::string failure_stage;
    std::uint64_t failure_frame = 0;
    std::uint64_t failure_latency_us = 0;
};

struct BrowserLiveRuntimeState {
    std::string active_mode = "none";
    std::string startup_state = "idle";
    bool starting = false;
    bool stopping = false;
    bool show_running_section = false;
    bool show_static_preview = false;
    bool show_idle_start_error = false;
    bool video_stream_source = false;
    BrowserRuntimeCapabilities runtime_capabilities{};
    BrowserLivePreviewState preview{};
    BrowserLiveControllerState controller{};
    BrowserLiveFanoutState fanout{};
    BrowserLiveAnalyzerState analyzer{};
    BrowserLiveManualOverlayState manual_overlay{};
    BrowserLiveCompositorState compositor{};
    BrowserLiveSingleBufferDiagnosticState single_buffer_diagnostic{};
    std::string start_error;
    std::string action_error;
};

template <typename Value>
struct BrowserNamedValue {
    std::string_view name;
    Value value;
};

template <typename Value, std::size_t N>
[[nodiscard]] inline std::optional<Value> find_browser_named_value(
    const std::string_view name, const std::array<BrowserNamedValue<Value>, N>& mappings) noexcept {
    for (const BrowserNamedValue<Value>& mapping : mappings) {
        if (mapping.name == name) {
            return mapping.value;
        }
    }
    return std::nullopt;
}

struct BrowserFileDialogStateAccess {
    mmltk::gui::TrainViewState* train = nullptr;
    mmltk::gui::PredictViewState* predict = nullptr;
    mmltk::gui::AnnotateViewState* annotate = nullptr;
    mmltk::gui::ValidateViewState* validate = nullptr;
    mmltk::gui::ExportViewState* export_state = nullptr;
};

struct BrowserFileDialogTarget {
    BrowserFileDialogTarget() = default;
    explicit BrowserFileDialogTarget(std::string* path_in) noexcept : path(path_in) {}
    BrowserFileDialogTarget(std::string* path_in, mmltk::gui::SourceSelectionState* source_in,
                            mmltk::gui::SourceKind source_kind_in) noexcept
        : path(path_in), source(source_in), source_kind(source_kind_in) {}

    std::string* path = nullptr;
    mmltk::gui::SourceSelectionState* source = nullptr;
    std::optional<mmltk::gui::SourceKind> source_kind;
};

[[nodiscard]] BrowserFileDialogTarget browser_file_dialog_target(BrowserFileDialogBinding binding,
                                                                 const BrowserFileDialogStateAccess& access) noexcept;

void apply_browser_file_dialog_selection(BrowserFileDialogBinding binding, std::string picked_path,
                                         const BrowserFileDialogStateAccess& access);

[[nodiscard]] mmltk::gui::AnnotationMaskCleanupOp annotation_mask_cleanup_op_from_browser_name(std::string_view name);

[[nodiscard]] Workflow workflow_from_view(mmltk::gui::View view) noexcept;
[[nodiscard]] CaptureRegion capture_region_from_live_region(const mmltk::live::LiveCaptureRegion& region) noexcept;
[[nodiscard]] SourceMetadata source_metadata_from_selection(const mmltk::gui::SourceSelectionState& source);
[[nodiscard]] mmltk::gui::AnnotationToolKind annotation_tool_kind_from_name(std::string_view name);
[[nodiscard]] AnnotationDocumentState annotation_document_state_from_snapshot(
    const mmltk::live::ManualOverlayDocumentSnapshot& snapshot);
[[nodiscard]] std::optional<AnnotationDocumentState> annotation_document_state_from_editor(
    const mmltk::gui::AnnotationFrame* frame, const mmltk::gui::AnnotationDocument* document,
    const mmltk::gui::AnnotationSession* session);
[[nodiscard]] nlohmann::json train_local_gpu_state_to_json(std::span<const mmltk::gui::LocalGpuInfo> gpus,
                                                           const std::vector<bool>& selected,
                                                           std::span<const int> configured_device_ids,
                                                           std::string_view error, bool refresh_running);
[[nodiscard]] nlohmann::json live_runtime_state_to_json(const BrowserLiveRuntimeState& state);
[[nodiscard]] const char* browser_live_display_startup_state(bool capture_running, std::string_view surface_revision,
                                                             std::string_view renderer_imported_revision,
                                                             std::string_view renderer_submitted_revision,
                                                             std::string_view renderer_drawn_revision) noexcept;
[[nodiscard]] nlohmann::json make_predict_start_settings_patch(const PredictStartIntent& intent);
[[nodiscard]] nlohmann::json make_annotate_save_settings_patch(const AnnotateSaveIntent& intent);
[[nodiscard]] BrowserFileDialogBinding file_dialog_binding_from_id(std::string_view dialog_id) noexcept;
[[nodiscard]] StateSnapshot make_state_snapshot(const GuiStateSnapshotInputs& inputs);
[[nodiscard]] bool apply_annotate_workspace_click(const AnnotateWorkspaceClickIntent& intent,
                                                  const BrowserAnnotateWorkspaceContext& context);
[[nodiscard]] bool apply_annotate_workspace_box_drag(const AnnotateWorkspaceBoxDragIntent& intent,
                                                     const BrowserAnnotateWorkspaceContext& context);
[[nodiscard]] bool apply_annotate_workspace_handle_drag(const AnnotateWorkspaceHandleDragIntent& intent,
                                                        const BrowserAnnotateWorkspaceContext& context);
[[nodiscard]] bool apply_annotate_workspace_brush(const AnnotateWorkspaceBrushIntent& intent,
                                                  const BrowserAnnotateWorkspaceContext& context);
[[nodiscard]] bool apply_annotate_workspace_fill(const AnnotateWorkspaceFillIntent& intent,
                                                 const BrowserAnnotateWorkspaceContext& context);
[[nodiscard]] bool apply_annotate_workspace_color_sample(const AnnotateWorkspaceColorSampleIntent& intent,
                                                         const BrowserAnnotateWorkspaceContext& context);
void apply_settings_update(const SettingsUpdateIntent& intent, mmltk::gui::GuiSettingsSnapshot& snapshot);
void apply_crop_commit(const CropCommitIntent& intent, mmltk::gui::SourceSelectionState& source) noexcept;
void apply_routed_intent(const RoutedIntent& intent, const BrowserHostIntentContext& context);

[[nodiscard]] FrameSlotDescriptor frame_slot_from_output_bundle(std::string slot_name, FrameTransportKind transport,
                                                                FramePixelFormat pixel_format,
                                                                const mmltk::live::OutputBundle& bundle);

[[nodiscard]] FrameSlotDescriptor frame_slot_from_workspace_output_bundle(
    std::string slot_name, FrameTransportKind transport, FramePixelFormat pixel_format,
    const mmltk::live::WorkspaceOutputBundle& bundle);

[[nodiscard]] FrameSlotDescriptor frame_slot_from_bgr_frame(std::string slot_name, std::uint32_t slot_index,
                                                            const mmltk::live::LiveFrameId& frame_id,
                                                            CaptureRegion capture_region, std::uint32_t width,
                                                            std::uint32_t height,
                                                            std::uint64_t published_row_stride_bytes,
                                                            std::uint64_t ready_ns, bool short_frame = false);

}  
