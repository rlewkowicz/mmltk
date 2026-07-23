#pragma once

#include "browser/api_contract_dsl.h"
#include "browser/browser_contract_metadata.h"
#include "browser/workflow_contract_generated.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::browser {

inline constexpr std::uint32_t kHostApiProtocolVersion = contract::kProtocolVersion;
inline constexpr std::string_view kStateSnapshotMessageType = "state.snapshot";
inline constexpr std::string_view kIntentMessageType = "intent";

enum class Workflow : std::uint8_t {
    Train = 0,
    Validate = 1,
    Predict = 2,
    Annotate = 3,
    Export = 4,
    Live = 5,
};

enum class SourceKind : std::uint8_t {
    CompiledDataset = 0,
    SingleImage = 1,
    ImageFolder = 2,
    VideoStream = 3,
};

enum class BrowserHostBackend : std::uint8_t {
    Unknown = 0,
    Firefox = 1,
};

enum class BrowserRuntimeCapabilityStatus : std::uint8_t {
    Unknown = 0,
    Available = 1,
    Unavailable = 2,
};

enum class BrowserBridgePhase : std::uint8_t {
    Idle = 0,
    Polling = 1,
    Dispatch = 2,
};

enum class ArtifactOperationPhase : std::uint8_t {
    Idle = 0,
    Running = 1,
    Complete = 2,
    Failed = 3,
    Cancelled = 4,
};

enum class DatasetCompilePhase : std::uint8_t {
    Idle = 0,
    Planning = 1,
    Labels = 2,
    Pixels = 3,
    Syncing = 4,
    Publishing = 5,
};

enum class WeightArtifactStatus : std::uint8_t {
    Idle = 0,
    Verifying = 1,
    Downloading = 2,
    RetryWaiting = 3,
    Ready = 4,
    NoConnection = 5,
    CannotDownload = 6,
    ChecksumError = 7,
    FilesystemError = 8,
    HttpError = 9,
    Incompatible = 10,
    Cancelled = 11,
};

enum class FileDialogMode : std::uint8_t {
    OpenFile = 0,
    OpenFolder = 1,
    SaveFile = 2,
};

enum class FileDialogResultStatus : std::uint8_t {
    Idle = 0,
    Pending = 1,
    Selected = 2,
    Cancelled = 3,
    Failed = 4,
};

enum class CustomModelArtifactKind : std::uint8_t {
    Weights = 0,
    Onnx = 1,
    TensorRt = 2,
};

enum class ModelPreflightStatus : std::uint8_t {
    Idle = 0,
    Verifying = 1,
    Ready = 2,
    Incompatible = 3,
    Failed = 4,
};

enum class AnnotateWorkspacePhase : std::uint8_t {
    Begin = 0,
    Update = 1,
    End = 2,
    Cancel = 3,
    Hover = 4,
};

enum class AnnotateHandleRole : std::uint8_t {
    Point = 0,
    SplineKnot = 1,
    SplineInHandle = 2,
    SplineOutHandle = 3,
    SkeletonNode = 4,
};

enum class AnnotateBoxDragKind : std::uint8_t {
    Create = 0,
    Move = 1,
    ResizeTopLeft = 2,
    ResizeTopRight = 3,
    ResizeBottomLeft = 4,
    ResizeBottomRight = 5,
};

enum class AnnotateSetupAction : std::uint8_t {
    StartLive = 0,
    StopLive = 1,
    ReloadFrame = 2,
    PrevFrame = 3,
    NextFrame = 4,
};

namespace api {

template <>
struct enum_traits<Workflow> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<Workflow>{"train", Workflow::Train},     enum_value<Workflow>{"validate", Workflow::Validate},
            enum_value<Workflow>{"predict", Workflow::Predict}, enum_value<Workflow>{"annotate", Workflow::Annotate},
            enum_value<Workflow>{"export", Workflow::Export},   enum_value<Workflow>{"live", Workflow::Live},
        };
    }
};

template <>
struct enum_traits<SourceKind> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<SourceKind>{"compiled_dataset", SourceKind::CompiledDataset},
            enum_value<SourceKind>{"single_image", SourceKind::SingleImage},
            enum_value<SourceKind>{"image_folder", SourceKind::ImageFolder},
            enum_value<SourceKind>{"video_stream", SourceKind::VideoStream},
        };
    }
};

template <>
struct enum_traits<BrowserHostBackend> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<BrowserHostBackend>{"unknown", BrowserHostBackend::Unknown},
            enum_value<BrowserHostBackend>{"firefox", BrowserHostBackend::Firefox},
        };
    }
};

template <>
struct enum_traits<BrowserRuntimeCapabilityStatus> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<BrowserRuntimeCapabilityStatus>{"unknown", BrowserRuntimeCapabilityStatus::Unknown},
            enum_value<BrowserRuntimeCapabilityStatus>{"available", BrowserRuntimeCapabilityStatus::Available},
            enum_value<BrowserRuntimeCapabilityStatus>{"unavailable", BrowserRuntimeCapabilityStatus::Unavailable},
        };
    }
};

template <>
struct enum_traits<BrowserBridgePhase> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<BrowserBridgePhase>{"idle", BrowserBridgePhase::Idle},
            enum_value<BrowserBridgePhase>{"polling", BrowserBridgePhase::Polling},
            enum_value<BrowserBridgePhase>{"dispatch", BrowserBridgePhase::Dispatch},
        };
    }
};

template <>
struct enum_traits<ArtifactOperationPhase> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<ArtifactOperationPhase>{"idle", ArtifactOperationPhase::Idle},
            enum_value<ArtifactOperationPhase>{"running", ArtifactOperationPhase::Running},
            enum_value<ArtifactOperationPhase>{"complete", ArtifactOperationPhase::Complete},
            enum_value<ArtifactOperationPhase>{"failed", ArtifactOperationPhase::Failed},
            enum_value<ArtifactOperationPhase>{"cancelled", ArtifactOperationPhase::Cancelled},
        };
    }
};

template <>
struct enum_traits<DatasetCompilePhase> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<DatasetCompilePhase>{"idle", DatasetCompilePhase::Idle},
            enum_value<DatasetCompilePhase>{"planning", DatasetCompilePhase::Planning},
            enum_value<DatasetCompilePhase>{"labels", DatasetCompilePhase::Labels},
            enum_value<DatasetCompilePhase>{"pixels", DatasetCompilePhase::Pixels},
            enum_value<DatasetCompilePhase>{"syncing", DatasetCompilePhase::Syncing},
            enum_value<DatasetCompilePhase>{"publishing", DatasetCompilePhase::Publishing},
        };
    }
};

template <>
struct enum_traits<WeightArtifactStatus> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<WeightArtifactStatus>{"idle", WeightArtifactStatus::Idle},
            enum_value<WeightArtifactStatus>{"verifying", WeightArtifactStatus::Verifying},
            enum_value<WeightArtifactStatus>{"downloading", WeightArtifactStatus::Downloading},
            enum_value<WeightArtifactStatus>{"retry_waiting", WeightArtifactStatus::RetryWaiting},
            enum_value<WeightArtifactStatus>{"ready", WeightArtifactStatus::Ready},
            enum_value<WeightArtifactStatus>{"no_connection", WeightArtifactStatus::NoConnection},
            enum_value<WeightArtifactStatus>{"cannot_download", WeightArtifactStatus::CannotDownload},
            enum_value<WeightArtifactStatus>{"checksum_error", WeightArtifactStatus::ChecksumError},
            enum_value<WeightArtifactStatus>{"filesystem_error", WeightArtifactStatus::FilesystemError},
            enum_value<WeightArtifactStatus>{"http_error", WeightArtifactStatus::HttpError},
            enum_value<WeightArtifactStatus>{"incompatible", WeightArtifactStatus::Incompatible},
            enum_value<WeightArtifactStatus>{"cancelled", WeightArtifactStatus::Cancelled},
        };
    }
};

template <>
struct enum_traits<FileDialogMode> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<FileDialogMode>{"open_file", FileDialogMode::OpenFile},
            enum_value<FileDialogMode>{"open_folder", FileDialogMode::OpenFolder},
            enum_value<FileDialogMode>{"save_file", FileDialogMode::SaveFile},
        };
    }
};

template <>
struct enum_traits<FileDialogResultStatus> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<FileDialogResultStatus>{"idle", FileDialogResultStatus::Idle},
            enum_value<FileDialogResultStatus>{"pending", FileDialogResultStatus::Pending},
            enum_value<FileDialogResultStatus>{"selected", FileDialogResultStatus::Selected},
            enum_value<FileDialogResultStatus>{"cancelled", FileDialogResultStatus::Cancelled},
            enum_value<FileDialogResultStatus>{"failed", FileDialogResultStatus::Failed},
        };
    }
};

template <>
struct enum_traits<CustomModelArtifactKind> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<CustomModelArtifactKind>{"weights", CustomModelArtifactKind::Weights},
            enum_value<CustomModelArtifactKind>{"onnx", CustomModelArtifactKind::Onnx},
            enum_value<CustomModelArtifactKind>{"tensorrt", CustomModelArtifactKind::TensorRt},
        };
    }
};

template <>
struct enum_traits<ModelPreflightStatus> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<ModelPreflightStatus>{"idle", ModelPreflightStatus::Idle},
            enum_value<ModelPreflightStatus>{"verifying", ModelPreflightStatus::Verifying},
            enum_value<ModelPreflightStatus>{"ready", ModelPreflightStatus::Ready},
            enum_value<ModelPreflightStatus>{"incompatible", ModelPreflightStatus::Incompatible},
            enum_value<ModelPreflightStatus>{"failed", ModelPreflightStatus::Failed},
        };
    }
};

template <>
struct enum_traits<AnnotateWorkspacePhase> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<AnnotateWorkspacePhase>{"begin", AnnotateWorkspacePhase::Begin},
            enum_value<AnnotateWorkspacePhase>{"update", AnnotateWorkspacePhase::Update},
            enum_value<AnnotateWorkspacePhase>{"end", AnnotateWorkspacePhase::End},
            enum_value<AnnotateWorkspacePhase>{"cancel", AnnotateWorkspacePhase::Cancel},
            enum_value<AnnotateWorkspacePhase>{"hover", AnnotateWorkspacePhase::Hover},
        };
    }
};

template <>
struct enum_traits<AnnotateHandleRole> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<AnnotateHandleRole>{"point", AnnotateHandleRole::Point},
            enum_value<AnnotateHandleRole>{"spline_knot", AnnotateHandleRole::SplineKnot},
            enum_value<AnnotateHandleRole>{"spline_in_handle", AnnotateHandleRole::SplineInHandle},
            enum_value<AnnotateHandleRole>{"spline_out_handle", AnnotateHandleRole::SplineOutHandle},
            enum_value<AnnotateHandleRole>{"skeleton_node", AnnotateHandleRole::SkeletonNode},
        };
    }
};

template <>
struct enum_traits<AnnotateBoxDragKind> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<AnnotateBoxDragKind>{"create", AnnotateBoxDragKind::Create},
            enum_value<AnnotateBoxDragKind>{"move", AnnotateBoxDragKind::Move},
            enum_value<AnnotateBoxDragKind>{"resize_top_left", AnnotateBoxDragKind::ResizeTopLeft},
            enum_value<AnnotateBoxDragKind>{"resize_top_right", AnnotateBoxDragKind::ResizeTopRight},
            enum_value<AnnotateBoxDragKind>{"resize_bottom_left", AnnotateBoxDragKind::ResizeBottomLeft},
            enum_value<AnnotateBoxDragKind>{"resize_bottom_right", AnnotateBoxDragKind::ResizeBottomRight},
        };
    }
};

template <>
struct enum_traits<AnnotateSetupAction> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<AnnotateSetupAction>{"start_live", AnnotateSetupAction::StartLive},
            enum_value<AnnotateSetupAction>{"stop_live", AnnotateSetupAction::StopLive},
            enum_value<AnnotateSetupAction>{"reload_frame", AnnotateSetupAction::ReloadFrame},
            enum_value<AnnotateSetupAction>{"prev_frame", AnnotateSetupAction::PrevFrame},
            enum_value<AnnotateSetupAction>{"next_frame", AnnotateSetupAction::NextFrame},
        };
    }
};

}  

struct CaptureRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&CaptureRegion::x>("x", api::required),
            api::field<&CaptureRegion::y>("y", api::required),
            api::field<&CaptureRegion::width>("width", api::required),
            api::field<&CaptureRegion::height>("height", api::required),
        };
    }
};

struct JobLogEntry {
    std::uint64_t sequence = 0;
    std::string level;
    std::string message;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&JobLogEntry::sequence>("sequence", api::required),
            api::field<&JobLogEntry::level>("level", api::required),
            api::field<&JobLogEntry::message>("message", api::required),
        };
    }
};

struct JobState {
    bool running = false;
    std::string label;
    std::string summary;
    std::string error;
    std::string output_tail;
    std::vector<JobLogEntry> recent_logs;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&JobState::running>("running"),         api::field<&JobState::label>("label"),
            api::field<&JobState::summary>("summary"),         api::field<&JobState::error>("error"),
            api::field<&JobState::output_tail>("output_tail"), api::field<&JobState::recent_logs>("recent_logs"),
        };
    }
};

struct SourceMetadata {
    SourceKind kind = SourceKind::CompiledDataset;
    std::string locator;
    bool recursive = false;
    int device_index = 0;
    int capture_width = 0;
    int capture_height = 0;
    int capture_fps = 0;
    int v4l2_buffer_count = 1;
    bool has_crop = false;
    CaptureRegion crop{};

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&SourceMetadata::kind>("kind", api::required),
            api::field<&SourceMetadata::locator>("locator"),
            api::field<&SourceMetadata::recursive>("recursive"),
            api::field<&SourceMetadata::device_index>("device_index"),
            api::field<&SourceMetadata::capture_width>("capture_width"),
            api::field<&SourceMetadata::capture_height>("capture_height"),
            api::field<&SourceMetadata::capture_fps>("capture_fps"),
            api::field<&SourceMetadata::v4l2_buffer_count>("v4l2_buffer_count"),
            api::field<&SourceMetadata::has_crop>("has_crop"),
            api::field<&SourceMetadata::crop>("crop"),
        };
    }
};

struct AnnotationDocumentState {
    struct Point {
        std::int32_t x = 0;
        std::int32_t y = 0;

        [[nodiscard]] static constexpr auto api_fields() {
            return std::tuple{api::field<&Point::x>("x"), api::field<&Point::y>("y")};
        }
    };

    struct Edge {
        std::uint32_t source_index = 0;
        std::uint32_t target_index = 0;

        [[nodiscard]] static constexpr auto api_fields() {
            return std::tuple{api::field<&Edge::source_index>("source_index"),
                              api::field<&Edge::target_index>("target_index")};
        }
    };

    struct Object {
        std::uint32_t object_index = 0;
        std::int32_t x1 = 0;
        std::int32_t y1 = 0;
        std::int32_t x2 = 0;
        std::int32_t y2 = 0;
        std::vector<Point> points;
        std::vector<Edge> edges;

        [[nodiscard]] static constexpr auto api_fields() {
            return std::tuple{api::field<&Object::object_index>("object_index"),
                              api::field<&Object::x1>("x1"),
                              api::field<&Object::y1>("y1"),
                              api::field<&Object::x2>("x2"),
                              api::field<&Object::y2>("y2"),
                              api::field<&Object::points>("points"),
                              api::field<&Object::edges>("edges")};
        }
    };

    struct Handle {
        std::uint32_t object_index = 0;
        std::uint32_t element_index = 0;
        AnnotateHandleRole role = AnnotateHandleRole::Point;
        std::int32_t x = 0;
        std::int32_t y = 0;

        [[nodiscard]] static constexpr auto api_fields() {
            return std::tuple{api::field<&Handle::object_index>("object_index"),
                              api::field<&Handle::element_index>("element_index"), api::field<&Handle::role>("role"),
                              api::field<&Handle::x>("x"), api::field<&Handle::y>("y")};
        }
    };

    std::uint64_t document_generation = 0;
    std::uint64_t session_revision = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::uint64_t instance_count = 0;
    std::optional<std::uint32_t> selected_instance;
    std::vector<Object> objects;
    std::vector<Handle> handles;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotationDocumentState::document_generation>("document_generation"),
            api::field<&AnnotationDocumentState::session_revision>("session_revision"),
            api::field<&AnnotationDocumentState::capture_width>("capture_width"),
            api::field<&AnnotationDocumentState::capture_height>("capture_height"),
            api::field<&AnnotationDocumentState::instance_count>("instance_count"),
            api::field<&AnnotationDocumentState::selected_instance>("selected_instance"),
            api::field<&AnnotationDocumentState::objects>("objects"),
            api::field<&AnnotationDocumentState::handles>("handles"),
        };
    }
};

struct WorkspaceSurfaceInfo {
    std::string generation;
    std::uint32_t capacity_width = 0;
    std::uint32_t capacity_height = 0;
    std::uint32_t slot_count = 0;
    std::string format = "rgba8unorm";
    std::string orientation = "upright";
    std::string source_kind = "unknown";
    bool ready = false;

    bool operator==(const WorkspaceSurfaceInfo&) const noexcept = default;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&WorkspaceSurfaceInfo::generation>("generation"),
            api::field<&WorkspaceSurfaceInfo::capacity_width>("capacityWidth"),
            api::field<&WorkspaceSurfaceInfo::capacity_height>("capacityHeight"),
            api::field<&WorkspaceSurfaceInfo::slot_count>("slotCount"),
            api::field<&WorkspaceSurfaceInfo::format>("format", api::enum_values(std::array{"rgba8unorm"})),
            api::field<&WorkspaceSurfaceInfo::orientation>("orientation", api::enum_values(std::array{"upright"})),
            api::field<&WorkspaceSurfaceInfo::source_kind>("sourceKind"),
            api::field<&WorkspaceSurfaceInfo::ready>("ready"),
        };
    }
};

struct BrowserRuntimeCapabilities {
    BrowserHostBackend host_backend = BrowserHostBackend::Unknown;
    BrowserRuntimeCapabilityStatus navigator_gpu = BrowserRuntimeCapabilityStatus::Unknown;
    BrowserRuntimeCapabilityStatus workspace_surface_bridge = BrowserRuntimeCapabilityStatus::Unknown;
    BrowserRuntimeCapabilityStatus workspace_surface_zero_copy = BrowserRuntimeCapabilityStatus::Unknown;

    bool operator==(const BrowserRuntimeCapabilities&) const noexcept = default;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&BrowserRuntimeCapabilities::host_backend>("host_backend"),
            api::field<&BrowserRuntimeCapabilities::navigator_gpu>("navigator_gpu"),
            api::field<&BrowserRuntimeCapabilities::workspace_surface_bridge>("workspace_surface_bridge"),
            api::field<&BrowserRuntimeCapabilities::workspace_surface_zero_copy>("workspace_surface_zero_copy"),
        };
    }
};

struct BrowserBridgeState {
    BrowserBridgePhase phase = BrowserBridgePhase::Polling;
    bool connected = false;
    std::string last_error;
    std::optional<std::uint64_t> last_success_revision;
    BrowserRuntimeCapabilities runtime_capabilities{};
    nlohmann::json capabilities = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&BrowserBridgeState::phase>("phase"),
            api::field<&BrowserBridgeState::connected>("connected"),
            api::field<&BrowserBridgeState::last_error>("last_error"),
            api::field<&BrowserBridgeState::last_success_revision>("last_success_revision"),
            api::field<&BrowserBridgeState::runtime_capabilities>("runtime_capabilities"),
        };
    }
};

struct ArtifactSplitState {
    std::string path;
    std::uint32_t image_count = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t channels = 0U;
    std::uint32_t class_count = 0U;
    std::vector<std::string> class_names;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ArtifactSplitState::path>("path"),
            api::field<&ArtifactSplitState::image_count>("image_count"),
            api::field<&ArtifactSplitState::width>("width"),
            api::field<&ArtifactSplitState::height>("height"),
            api::field<&ArtifactSplitState::channels>("channels"),
            api::field<&ArtifactSplitState::class_count>("class_count"),
            api::field<&ArtifactSplitState::class_names>("class_names"),
        };
    }
};

struct DatasetArtifactState {
    ArtifactOperationPhase phase = ArtifactOperationPhase::Idle;
    DatasetCompilePhase compile_phase = DatasetCompilePhase::Idle;
    std::uint64_t generation = 0U;
    std::string source_dir;
    std::string output_dir;
    std::string preset_name;
    std::string active_split;
    std::uint64_t done = 0U;
    std::uint64_t total = 0U;
    std::uint64_t elapsed_ms = 0U;
    std::uint64_t remaining_ms = 0U;
    std::uint64_t dropped_instances = 0U;
    bool eta_ready = false;
    bool compatible = false;
    bool compiling = false;
    std::string error;
    std::vector<ArtifactSplitState> splits;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&DatasetArtifactState::phase>("phase"),
            api::field<&DatasetArtifactState::compile_phase>("compile_phase"),
            api::field<&DatasetArtifactState::generation>("generation"),
            api::field<&DatasetArtifactState::source_dir>("source_dir"),
            api::field<&DatasetArtifactState::output_dir>("output_dir"),
            api::field<&DatasetArtifactState::preset_name>("preset_name"),
            api::field<&DatasetArtifactState::active_split>("active_split"),
            api::field<&DatasetArtifactState::done>("done"),
            api::field<&DatasetArtifactState::total>("total"),
            api::field<&DatasetArtifactState::elapsed_ms>("elapsed_ms"),
            api::field<&DatasetArtifactState::remaining_ms>("remaining_ms"),
            api::field<&DatasetArtifactState::dropped_instances>("dropped_instances"),
            api::field<&DatasetArtifactState::eta_ready>("eta_ready"),
            api::field<&DatasetArtifactState::compatible>("compatible"),
            api::field<&DatasetArtifactState::compiling>("compiling"),
            api::field<&DatasetArtifactState::error>("error"),
            api::field<&DatasetArtifactState::splits>("splits"),
        };
    }
};

struct WeightArtifactState {
    ArtifactOperationPhase phase = ArtifactOperationPhase::Idle;
    WeightArtifactStatus status = WeightArtifactStatus::Idle;
    std::uint64_t generation = 0U;
    std::string preset_name;
    std::string path;
    std::string error;
    std::uint64_t downloaded_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    std::uint32_t attempt = 0U;
    std::uint64_t retry_after_ms = 0U;
    bool resumable = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&WeightArtifactState::phase>("phase"),
            api::field<&WeightArtifactState::status>("status"),
            api::field<&WeightArtifactState::generation>("generation"),
            api::field<&WeightArtifactState::preset_name>("preset_name"),
            api::field<&WeightArtifactState::path>("path"),
            api::field<&WeightArtifactState::error>("error"),
            api::field<&WeightArtifactState::downloaded_bytes>("downloaded_bytes"),
            api::field<&WeightArtifactState::total_bytes>("total_bytes"),
            api::field<&WeightArtifactState::attempt>("attempt"),
            api::field<&WeightArtifactState::retry_after_ms>("retry_after_ms"),
            api::field<&WeightArtifactState::resumable>("resumable"),
        };
    }
};

struct ArtifactState {
    DatasetArtifactState dataset;
    WeightArtifactState weight;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ArtifactState::dataset>("dataset"),
            api::field<&ArtifactState::weight>("weight"),
        };
    }
};

struct FileDialogState {
    std::uint64_t token = 0U;
    FileDialogResultStatus status = FileDialogResultStatus::Idle;
    std::string dialog_id;
    std::string path;
    std::string error;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&FileDialogState::token>("token"),         api::field<&FileDialogState::status>("status"),
            api::field<&FileDialogState::dialog_id>("dialog_id"), api::field<&FileDialogState::path>("path"),
            api::field<&FileDialogState::error>("error"),
        };
    }
};

struct ModelPreflightState {
    std::uint64_t generation = 0U;
    Workflow workflow = Workflow::Train;
    ModelPreflightStatus status = ModelPreflightStatus::Idle;
    CustomModelArtifactKind artifact_kind = CustomModelArtifactKind::Weights;
    std::string preset_name;
    std::uint32_t resolution = 0U;
    std::string path;
    std::string error;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ModelPreflightState::generation>("generation"),
            api::field<&ModelPreflightState::workflow>("workflow"),
            api::field<&ModelPreflightState::status>("status"),
            api::field<&ModelPreflightState::artifact_kind>("artifact_kind"),
            api::field<&ModelPreflightState::preset_name>("preset_name"),
            api::field<&ModelPreflightState::resolution>("resolution"),
            api::field<&ModelPreflightState::path>("path"),
            api::field<&ModelPreflightState::error>("error"),
        };
    }
};

struct StateSnapshot {
    std::uint32_t protocol_version = kHostApiProtocolVersion;
    std::string contract_hash = std::string(kBrowserUiContractHash);
    std::uint64_t state_revision = 0;
    Workflow active_workflow = Workflow::Train;
    nlohmann::json workflow_state = nlohmann::json::object();
    nlohmann::json settings_state = nlohmann::json::object();
    JobState job{};
    SourceMetadata source{};
    AnnotationDocumentState annotation{};
    BrowserRuntimeCapabilities runtime_capabilities{};
    ArtifactState artifacts{};
    FileDialogState file_dialog{};
    ModelPreflightState model_preflight{};
    std::optional<WorkspaceSurfaceInfo> workspace_surface;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&StateSnapshot::protocol_version>("protocol_version"),
            api::field<&StateSnapshot::contract_hash>("contract_hash"),
            api::field<&StateSnapshot::state_revision>("state_revision"),
            api::field<&StateSnapshot::active_workflow>("active_workflow"),
            api::field<&StateSnapshot::workflow_state>("workflow_state", api::additional_properties(true)),
            api::field<&StateSnapshot::settings_state>("settings_state", api::additional_properties(true)),
            api::field<&StateSnapshot::job>("job"),
            api::field<&StateSnapshot::source>("source"),
            api::field<&StateSnapshot::annotation>("annotation"),
            api::field<&StateSnapshot::runtime_capabilities>("runtime_capabilities"),
            api::field<&StateSnapshot::artifacts>("artifacts"),
            api::field<&StateSnapshot::file_dialog>("file_dialog"),
            api::field<&StateSnapshot::model_preflight>("model_preflight"),
            api::field<&StateSnapshot::workspace_surface>("workspace_surface"),
        };
    }
};

[[nodiscard]] std::string_view workflow_name(Workflow workflow) noexcept;
[[nodiscard]] Workflow workflow_from_name(std::string_view name);

[[nodiscard]] std::string_view source_kind_name(SourceKind kind) noexcept;
[[nodiscard]] SourceKind source_kind_from_name(std::string_view name);

[[nodiscard]] std::string_view browser_host_backend_name(BrowserHostBackend backend) noexcept;
[[nodiscard]] BrowserHostBackend browser_host_backend_from_name(std::string_view name);

[[nodiscard]] std::string_view browser_runtime_capability_status_name(BrowserRuntimeCapabilityStatus status) noexcept;
[[nodiscard]] BrowserRuntimeCapabilityStatus browser_runtime_capability_status_from_name(std::string_view name);

[[nodiscard]] std::string_view browser_bridge_phase_name(BrowserBridgePhase phase) noexcept;
[[nodiscard]] BrowserBridgePhase browser_bridge_phase_from_name(std::string_view name);

[[nodiscard]] std::string_view artifact_operation_phase_name(ArtifactOperationPhase phase) noexcept;
[[nodiscard]] ArtifactOperationPhase artifact_operation_phase_from_name(std::string_view name);

[[nodiscard]] std::string_view file_dialog_mode_name(FileDialogMode mode) noexcept;
[[nodiscard]] FileDialogMode file_dialog_mode_from_name(std::string_view name);

void to_json(nlohmann::json& j, const CaptureRegion& region);
void from_json(const nlohmann::json& j, CaptureRegion& region);

void to_json(nlohmann::json& j, const JobLogEntry& entry);
void from_json(const nlohmann::json& j, JobLogEntry& entry);

void to_json(nlohmann::json& j, const JobState& state);
void from_json(const nlohmann::json& j, JobState& state);

void to_json(nlohmann::json& j, const ArtifactSplitState& state);
void from_json(const nlohmann::json& j, ArtifactSplitState& state);
void to_json(nlohmann::json& j, const DatasetArtifactState& state);
void from_json(const nlohmann::json& j, DatasetArtifactState& state);
void to_json(nlohmann::json& j, const WeightArtifactState& state);
void from_json(const nlohmann::json& j, WeightArtifactState& state);
void to_json(nlohmann::json& j, const ArtifactState& state);
void from_json(const nlohmann::json& j, ArtifactState& state);
void to_json(nlohmann::json& j, const FileDialogState& state);
void from_json(const nlohmann::json& j, FileDialogState& state);
void to_json(nlohmann::json& j, const ModelPreflightState& state);
void from_json(const nlohmann::json& j, ModelPreflightState& state);

void to_json(nlohmann::json& j, const SourceMetadata& metadata);
void from_json(const nlohmann::json& j, SourceMetadata& metadata);

void to_json(nlohmann::json& j, const AnnotationDocumentState& state);
void from_json(const nlohmann::json& j, AnnotationDocumentState& state);

void to_json(nlohmann::json& j, const WorkspaceSurfaceInfo& surface_info);
void from_json(const nlohmann::json& j, WorkspaceSurfaceInfo& surface_info);

void to_json(nlohmann::json& j, const BrowserRuntimeCapabilities& capabilities);
void from_json(const nlohmann::json& j, BrowserRuntimeCapabilities& capabilities);

void to_json(nlohmann::json& j, const StateSnapshot& snapshot);
void from_json(const nlohmann::json& j, StateSnapshot& snapshot);

}  
