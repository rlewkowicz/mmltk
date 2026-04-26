#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::browser {

inline constexpr std::uint32_t kHostApiProtocolVersion = 2U;
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

struct CaptureRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct JobLogEntry {
    std::uint64_t sequence = 0;
    std::string level;
    std::string message;
};

struct JobState {
    bool running = false;
    std::string label;
    std::string summary;
    std::string error;
    std::string output_tail;
    std::vector<JobLogEntry> recent_logs;
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
};

struct AnnotationDocumentState {
    std::uint64_t document_generation = 0;
    std::uint64_t session_revision = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    std::uint64_t instance_count = 0;
    std::optional<std::uint32_t> selected_instance;
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
};

struct BrowserRuntimeCapabilities {
    BrowserHostBackend host_backend = BrowserHostBackend::Unknown;
    BrowserRuntimeCapabilityStatus navigator_gpu = BrowserRuntimeCapabilityStatus::Unknown;
    BrowserRuntimeCapabilityStatus workspace_surface_bridge = BrowserRuntimeCapabilityStatus::Unknown;
    BrowserRuntimeCapabilityStatus workspace_surface_zero_copy = BrowserRuntimeCapabilityStatus::Unknown;

    bool operator==(const BrowserRuntimeCapabilities&) const noexcept = default;
};

struct BrowserBridgeState {
    BrowserBridgePhase phase = BrowserBridgePhase::Polling;
    bool connected = false;
    std::string last_error;
    std::optional<std::uint64_t> last_success_revision;
    BrowserRuntimeCapabilities runtime_capabilities{};
};

struct StateSnapshot {
    std::uint32_t protocol_version = kHostApiProtocolVersion;
    std::uint64_t state_revision = 0;
    Workflow active_workflow = Workflow::Train;
    nlohmann::json workflow_state = nlohmann::json::object();
    nlohmann::json settings_state = nlohmann::json::object();
    JobState job{};
    SourceMetadata source{};
    AnnotationDocumentState annotation{};
    BrowserRuntimeCapabilities runtime_capabilities{};
    std::optional<WorkspaceSurfaceInfo> workspace_surface;
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
