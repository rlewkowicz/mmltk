#pragma once

#include "browser/host_api_protocol.h"

#include <variant>

namespace mmltk::browser {

struct IntentMessage {
    std::uint32_t protocol_version = kHostApiProtocolVersion;
    std::uint64_t request_id = 0;
    Workflow workflow = Workflow::Train;
    std::string intent;
    nlohmann::json payload = nlohmann::json::object();
};

struct TrainStartIntent {
    nlohmann::json options = nlohmann::json::object();
};

struct TrainStopIntent {
    bool force = false;
};

struct TrainRemoteQueryIntent {};

struct TrainLocalGpuRefreshIntent {};

struct TrainRemoteOfferArmIntent {
    int offer_id = 0;
};

struct TrainRemoteOfferClearIntent {};

struct TrainRemoteStartIntent {
    nlohmann::json options = nlohmann::json::object();
};

struct TrainRemoteStopIntent {};

struct PredictStartIntent {
    std::optional<std::string> selected_preset;
    nlohmann::json options = nlohmann::json::object();
};

struct PredictStopIntent {};

struct ValidateStartIntent {
    nlohmann::json options = nlohmann::json::object();
};

struct ExportStartIntent {
    nlohmann::json options = nlohmann::json::object();
};

struct AnnotateSaveIntent {
    nlohmann::json options = nlohmann::json::object();
};

struct AnnotateSetupIntent {
    AnnotateSetupAction action = AnnotateSetupAction::ReloadFrame;
};

struct AnnotateHoldSaveIntent {
    bool enabled = false;
};

struct AnnotateBrushRadiusIntent {
    int radius = 1;
};

struct AnnotateSidebarIntent {
    std::string action;
    std::optional<std::uint32_t> object_index;
    std::optional<std::uint32_t> category_index;
    std::optional<bool> enabled;
    std::string category_name;
    std::string tool;
    std::optional<std::uint32_t> spline_segment_index;
    std::string spline_handle_mode;
    std::optional<std::uint32_t> skeleton_joint_index;
    std::optional<int> cleanup_radius;
    std::string cleanup_op;
    nlohmann::json sup = nlohmann::json::object();
    nlohmann::json nosup = nlohmann::json::object();
};

struct AnnotateWorkspaceHandleRef {
    std::uint32_t object_index = 0;
    std::uint32_t element_index = 0;
    AnnotateHandleRole role = AnnotateHandleRole::Point;
};

struct AnnotateWorkspaceClickIntent {
    int capture_x = 0;
    int capture_y = 0;
    bool double_click = false;
};

struct AnnotateWorkspaceBoxDragIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    std::optional<AnnotateBoxDragKind> drag_kind;
    std::optional<std::uint32_t> object_index;
    int capture_x = 0;
    int capture_y = 0;
};

struct AnnotateWorkspaceHandleDragIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    std::optional<AnnotateWorkspaceHandleRef> handle;
    int capture_x = 0;
    int capture_y = 0;
};

struct AnnotateWorkspaceBrushIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    int capture_x = 0;
    int capture_y = 0;
    int radius = 1;
};

struct AnnotateWorkspaceFillIntent {
    int capture_x = 0;
    int capture_y = 0;
};

struct AnnotateWorkspaceColorSampleIntent {
    int capture_x = 0;
    int capture_y = 0;
};

struct ToolSelectIntent {
    std::string tool;
    nlohmann::json options = nlohmann::json::object();
};

struct LivePreviewControlIntent {
    std::optional<bool> fit_to_capture;
    std::optional<bool> crop_overlay_mode;
};

struct SettingsUpdateIntent {
    nlohmann::json patch = nlohmann::json::object();
};

struct CropCommitIntent {
    bool has_crop = false;
    CaptureRegion crop{};
};

struct FileDialogFilter {
    std::string name;
    std::vector<std::string> patterns;
};

struct FileDialogRequestIntent {
    std::string dialog_id;
    FileDialogMode mode = FileDialogMode::OpenFile;
    std::string title;
    std::string default_path;
    std::vector<FileDialogFilter> filters;
};

struct ViewportCommitIntent {
    double center_x = 0.0;
    double center_y = 0.0;
    double zoom = 1.0;
    std::optional<CaptureRegion> clip;
};

using RoutedIntentPayload =
    std::variant<TrainStartIntent, TrainStopIntent, TrainRemoteQueryIntent, TrainLocalGpuRefreshIntent,
                 TrainRemoteOfferArmIntent, TrainRemoteOfferClearIntent, TrainRemoteStartIntent, TrainRemoteStopIntent,
                 PredictStartIntent, PredictStopIntent, ValidateStartIntent, ExportStartIntent, AnnotateSaveIntent,
                 AnnotateSetupIntent, AnnotateHoldSaveIntent, AnnotateBrushRadiusIntent, AnnotateSidebarIntent,
                 AnnotateWorkspaceClickIntent, AnnotateWorkspaceBoxDragIntent, AnnotateWorkspaceHandleDragIntent,
                 AnnotateWorkspaceBrushIntent, AnnotateWorkspaceFillIntent, AnnotateWorkspaceColorSampleIntent,
                 ToolSelectIntent, LivePreviewControlIntent, SettingsUpdateIntent, CropCommitIntent,
                 FileDialogRequestIntent, ViewportCommitIntent>;
inline constexpr std::size_t kRoutedIntentPayloadAlternativeCount = std::variant_size_v<RoutedIntentPayload>;

struct RoutedIntent {
    std::uint32_t protocol_version = kHostApiProtocolVersion;
    std::uint64_t request_id = 0;
    Workflow workflow = Workflow::Train;
    RoutedIntentPayload payload;
};

[[nodiscard]] std::string_view routed_intent_name(const RoutedIntentPayload& payload) noexcept;
[[nodiscard]] RoutedIntent route_intent(const IntentMessage& intent);

void to_json(nlohmann::json& j, const IntentMessage& intent);
void from_json(const nlohmann::json& j, IntentMessage& intent);

}  // namespace mmltk::browser
