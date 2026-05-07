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
    Cef = 1,
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

enum class FileDialogMode : std::uint8_t {
    OpenFile = 0,
    OpenFolder = 1,
    SaveFile = 2,
};

enum class AnnotateWorkspacePhase : std::uint8_t {
    Begin = 0,
    Update = 1,
    End = 2,
    Cancel = 3,
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
            enum_value<BrowserHostBackend>{"cef", BrowserHostBackend::Cef},
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
struct enum_traits<AnnotateWorkspacePhase> {
    [[nodiscard]] static constexpr auto values() {
        return std::array{
            enum_value<AnnotateWorkspacePhase>{"begin", AnnotateWorkspacePhase::Begin},
            enum_value<AnnotateWorkspacePhase>{"update", AnnotateWorkspacePhase::Update},
            enum_value<AnnotateWorkspacePhase>{"end", AnnotateWorkspacePhase::End},
            enum_value<AnnotateWorkspacePhase>{"cancel", AnnotateWorkspacePhase::Cancel},
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

}  // namespace api

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
    std::uint64_t document_generation = 0;
    std::uint64_t session_revision = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::uint64_t instance_count = 0;
    std::optional<std::uint32_t> selected_instance;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotationDocumentState::document_generation>("document_generation"),
            api::field<&AnnotationDocumentState::session_revision>("session_revision"),
            api::field<&AnnotationDocumentState::capture_width>("capture_width"),
            api::field<&AnnotationDocumentState::capture_height>("capture_height"),
            api::field<&AnnotationDocumentState::instance_count>("instance_count"),
            api::field<&AnnotationDocumentState::selected_instance>("selected_instance"),
        };
    }
};

struct WorkspaceSurfaceInfo {
    std::string surface_id = "workspace";
    std::string revision;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string texture_format = "rgba8unorm";
    bool opaque = true;
    bool upright = true;

    bool operator==(const WorkspaceSurfaceInfo&) const noexcept = default;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&WorkspaceSurfaceInfo::surface_id>("surfaceId", api::enum_values(std::array{"workspace"})),
            api::field<&WorkspaceSurfaceInfo::revision>("revision"),
            api::field<&WorkspaceSurfaceInfo::width>("width"),
            api::field<&WorkspaceSurfaceInfo::height>("height"),
            api::field<&WorkspaceSurfaceInfo::texture_format>("textureFormat",
                                                              api::enum_values(std::array{"rgba8unorm"})),
            api::field<&WorkspaceSurfaceInfo::opaque>("opaque"),
            api::field<&WorkspaceSurfaceInfo::upright>("upright"),
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

[[nodiscard]] std::string_view file_dialog_mode_name(FileDialogMode mode) noexcept;
[[nodiscard]] FileDialogMode file_dialog_mode_from_name(std::string_view name);

void to_json(nlohmann::json& j, const CaptureRegion& region);
void from_json(const nlohmann::json& j, CaptureRegion& region);

void to_json(nlohmann::json& j, const JobLogEntry& entry);
void from_json(const nlohmann::json& j, JobLogEntry& entry);

void to_json(nlohmann::json& j, const JobState& state);
void from_json(const nlohmann::json& j, JobState& state);

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

}  // namespace mmltk::browser
