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

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&IntentMessage::protocol_version>("protocol_version"),
            api::field<&IntentMessage::request_id>("request_id"),
            api::field<&IntentMessage::workflow>("workflow", api::required),
            api::field<&IntentMessage::intent>("intent", api::required),
            api::field<&IntentMessage::payload>("payload", api::additional_properties(true)),
        };
    }
};

struct TrainStartIntent {
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&TrainStartIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.start", api::workflow_set(Workflow::Train));
    }
};

struct TrainStopIntent {
    bool force = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&TrainStopIntent::force>("force"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.stop", api::workflow_set(Workflow::Train), "TrainStopIntent");
    }
};

struct TrainRemoteQueryIntent {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.remote.query", api::workflow_set(Workflow::Train), "EmptyIntentPayload");
    }
};

struct TrainLocalGpuRefreshIntent {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.local_gpu.refresh", api::workflow_set(Workflow::Train), "EmptyIntentPayload");
    }
};

struct DatasetCompileStartIntent {
    std::string source_dir;
    std::string output_dir;
    std::string preset_name;
    std::uint32_t resolution = 0U;
    bool overwrite = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&DatasetCompileStartIntent::source_dir>("source_dir", api::required),
            api::field<&DatasetCompileStartIntent::output_dir>("output_dir", api::required),
            api::field<&DatasetCompileStartIntent::preset_name>("preset_name", api::required),
            api::field<&DatasetCompileStartIntent::resolution>("resolution", api::required, api::minimum(1)),
            api::field<&DatasetCompileStartIntent::overwrite>("overwrite", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("dataset.compile.start", api::workflow_set(Workflow::Train), "DatasetCompileStartIntent");
    }
};

struct DatasetCompileCancelIntent {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("dataset.compile.cancel", api::workflow_set(Workflow::Train), "EmptyIntentPayload");
    }
};

struct CustomModelSelectIntent {
    std::uint64_t dialog_token = 0U;
    CustomModelArtifactKind artifact_kind = CustomModelArtifactKind::Weights;
    std::string preset_name;
    std::uint32_t resolution = 0U;
    std::string path;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&CustomModelSelectIntent::dialog_token>("dialog_token", api::required),
            api::field<&CustomModelSelectIntent::artifact_kind>("artifact_kind", api::required),
            api::field<&CustomModelSelectIntent::preset_name>("preset_name", api::required),
            api::field<&CustomModelSelectIntent::resolution>("resolution", api::required, api::minimum(1)),
            api::field<&CustomModelSelectIntent::path>("path", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("model.custom.select",
                           api::workflow_set(Workflow::Train, Workflow::Validate, Workflow::Predict, Workflow::Annotate,
                                             Workflow::Export, Workflow::Live),
                           "CustomModelSelectIntent");
    }
};

struct SettingsResetIntent {
    bool confirmed = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&SettingsResetIntent::confirmed>("confirmed", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("settings.reset",
                           api::workflow_set(Workflow::Train, Workflow::Validate, Workflow::Predict, Workflow::Annotate,
                                             Workflow::Export, Workflow::Live),
                           "SettingsResetIntent");
    }
};

struct TrainRemoteOfferArmIntent {
    int offer_id = 0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&TrainRemoteOfferArmIntent::offer_id>("offer_id", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.remote.offer.arm", api::workflow_set(Workflow::Train), "TrainRemoteOfferArmIntent");
    }
};

struct TrainRemoteOfferClearIntent {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.remote.offer.clear", api::workflow_set(Workflow::Train), "EmptyIntentPayload");
    }
};

struct TrainRemoteStartIntent {
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&TrainRemoteStartIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.remote.start", api::workflow_set(Workflow::Train));
    }
};

struct TrainRemoteStopIntent {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("train.remote.stop", api::workflow_set(Workflow::Train), "EmptyIntentPayload");
    }
};

struct PredictStartIntentPayload {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr bool api_additional_properties() {
        return true;
    }
};

struct PredictStartIntent {
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&PredictStartIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("predict.start", api::workflow_set(Workflow::Predict, Workflow::Live), "PredictStartIntent");
    }
};

struct EmptyIntentPayload {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }
};

struct PredictStopIntent {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("predict.stop", api::workflow_set(Workflow::Predict, Workflow::Live), "EmptyIntentPayload");
    }
};

struct ValidateStartIntent {
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ValidateStartIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("validate.start", api::workflow_set(Workflow::Validate));
    }
};

struct ExportStartIntent {
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ExportStartIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("export.start", api::workflow_set(Workflow::Export));
    }
};

struct AnnotateSaveIntent {
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateSaveIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.save", api::workflow_set(Workflow::Annotate));
    }

    [[nodiscard]] static constexpr auto api_intents() {
        return std::tuple{
            api::intent("annotate.save", api::workflow_set(Workflow::Annotate)),
            api::intent("annotate.save_now", api::workflow_set(Workflow::Annotate)),
        };
    }
};

struct AnnotateLiveStartIntentPayload {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }
};

struct AnnotateLiveStopIntentPayload {
    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{};
    }
};

struct AnnotateSetupIntent {
    struct ActionForIntent {
        std::string_view id;
        AnnotateSetupAction action;
    };

    AnnotateSetupAction action = AnnotateSetupAction::ReloadFrame;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateSetupIntent::action>("action", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.setup_frame.reload", api::workflow_set(Workflow::Annotate), "EmptyIntentPayload");
    }

    [[nodiscard]] static constexpr auto api_intents() {
        return std::tuple{
            api::intent("annotate.live.start", api::workflow_set(Workflow::Annotate), "AnnotateLiveStartIntent"),
            api::intent("annotate.live.stop", api::workflow_set(Workflow::Annotate), "AnnotateLiveStopIntent"),
            api::intent("annotate.setup_frame.reload", api::workflow_set(Workflow::Annotate), "EmptyIntentPayload"),
            api::intent("annotate.setup_frame.prev", api::workflow_set(Workflow::Annotate), "EmptyIntentPayload"),
            api::intent("annotate.setup_frame.next", api::workflow_set(Workflow::Annotate), "EmptyIntentPayload"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent_actions() {
        return std::array{
            ActionForIntent{"annotate.live.start", AnnotateSetupAction::StartLive},
            ActionForIntent{"annotate.live.stop", AnnotateSetupAction::StopLive},
            ActionForIntent{"annotate.setup_frame.reload", AnnotateSetupAction::ReloadFrame},
            ActionForIntent{"annotate.setup_frame.prev", AnnotateSetupAction::PrevFrame},
            ActionForIntent{"annotate.setup_frame.next", AnnotateSetupAction::NextFrame},
        };
    }
};

struct AnnotateHoldSaveIntent {
    bool enabled = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateHoldSaveIntent::enabled>("enabled", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.hold_save", api::workflow_set(Workflow::Annotate), "AnnotateHoldSaveIntent");
    }
};

struct AnnotateBrushRadiusIntent {
    int radius = 1;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateBrushRadiusIntent::radius>("radius", api::required, api::minimum(1)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.brush_radius", api::workflow_set(Workflow::Annotate), "AnnotateBrushRadiusIntent");
    }
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

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateSidebarIntent::action>("action", api::required),
            api::field<&AnnotateSidebarIntent::object_index>("object_index"),
            api::field<&AnnotateSidebarIntent::category_index>("category_index"),
            api::field<&AnnotateSidebarIntent::enabled>("enabled"),
            api::field<&AnnotateSidebarIntent::category_name>("category_name"),
            api::field<&AnnotateSidebarIntent::tool>("tool"),
            api::field<&AnnotateSidebarIntent::spline_segment_index>("spline_segment_index"),
            api::field<&AnnotateSidebarIntent::spline_handle_mode>("spline_handle_mode"),
            api::field<&AnnotateSidebarIntent::skeleton_joint_index>("skeleton_joint_index"),
            api::field<&AnnotateSidebarIntent::cleanup_radius>("cleanup_radius"),
            api::field<&AnnotateSidebarIntent::cleanup_op>("cleanup_op"),
            api::field<&AnnotateSidebarIntent::sup>("sup", api::additional_properties(true)),
            api::field<&AnnotateSidebarIntent::nosup>("nosup", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.sidebar", api::workflow_set(Workflow::Annotate), "AnnotateSidebarIntent");
    }
};

struct AnnotateWorkspaceHandleRef {
    std::uint32_t object_index = 0;
    std::uint32_t element_index = 0;
    AnnotateHandleRole role = AnnotateHandleRole::Point;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceHandleRef::object_index>("object_index", api::required),
            api::field<&AnnotateWorkspaceHandleRef::element_index>("element_index", api::required),
            api::field<&AnnotateWorkspaceHandleRef::role>("role", api::required),
        };
    }
};

struct AnnotateWorkspaceClickIntent {
    int capture_x = 0;
    int capture_y = 0;
    bool double_click = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceClickIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspaceClickIntent::capture_y>("capture_y", api::required),
            api::field<&AnnotateWorkspaceClickIntent::double_click>("double_click"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.click", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspaceClickIntent");
    }
};

struct AnnotateWorkspaceBoxDragIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    std::optional<AnnotateBoxDragKind> drag_kind;
    std::optional<std::uint32_t> object_index;
    int capture_x = 0;
    int capture_y = 0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceBoxDragIntent::phase>("phase", api::required),
            api::field<&AnnotateWorkspaceBoxDragIntent::drag_kind>("drag_kind"),
            api::field<&AnnotateWorkspaceBoxDragIntent::object_index>("object_index"),
            api::field<&AnnotateWorkspaceBoxDragIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspaceBoxDragIntent::capture_y>("capture_y", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.box_drag", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspaceBoxDragIntent");
    }
};

struct AnnotateWorkspaceHandleDragIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    std::optional<AnnotateWorkspaceHandleRef> handle;
    int capture_x = 0;
    int capture_y = 0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceHandleDragIntent::phase>("phase", api::required),
            api::field<&AnnotateWorkspaceHandleDragIntent::handle>("handle"),
            api::field<&AnnotateWorkspaceHandleDragIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspaceHandleDragIntent::capture_y>("capture_y", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.handle_drag", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspaceHandleDragIntent");
    }
};

struct AnnotateWorkspaceBrushIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    int capture_x = 0;
    int capture_y = 0;
    int radius = 1;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceBrushIntent::phase>("phase", api::required),
            api::field<&AnnotateWorkspaceBrushIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspaceBrushIntent::capture_y>("capture_y", api::required),
            api::field<&AnnotateWorkspaceBrushIntent::radius>("radius", api::required, api::minimum(1)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.brush", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspaceBrushIntent");
    }
};

struct AnnotateWorkspacePointerIntent {
    AnnotateWorkspacePhase phase = AnnotateWorkspacePhase::Begin;
    std::uint32_t pointer_id = 0;
    int button = 0;
    std::uint32_t buttons = 0;
    double canvas_x = 0.0;
    double canvas_y = 0.0;
    int capture_x = 0;
    int capture_y = 0;
    std::string tool;
    int brush_radius = 1;
    bool erase = false;
    bool shift_key = false;
    bool ctrl_key = false;
    bool alt_key = false;
    bool meta_key = false;
    std::optional<AnnotateBoxDragKind> drag_kind;
    std::optional<std::uint32_t> object_index;
    std::optional<AnnotateWorkspaceHandleRef> handle;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspacePointerIntent::phase>("phase", api::required),
            api::field<&AnnotateWorkspacePointerIntent::pointer_id>("pointer_id", api::required),
            api::field<&AnnotateWorkspacePointerIntent::button>("button", api::required),
            api::field<&AnnotateWorkspacePointerIntent::buttons>("buttons", api::required),
            api::field<&AnnotateWorkspacePointerIntent::canvas_x>("canvas_x", api::required),
            api::field<&AnnotateWorkspacePointerIntent::canvas_y>("canvas_y", api::required),
            api::field<&AnnotateWorkspacePointerIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspacePointerIntent::capture_y>("capture_y", api::required),
            api::field<&AnnotateWorkspacePointerIntent::tool>("tool", api::required),
            api::field<&AnnotateWorkspacePointerIntent::brush_radius>("brush_radius", api::required, api::minimum(1)),
            api::field<&AnnotateWorkspacePointerIntent::erase>("erase"),
            api::field<&AnnotateWorkspacePointerIntent::shift_key>("shift_key"),
            api::field<&AnnotateWorkspacePointerIntent::ctrl_key>("ctrl_key"),
            api::field<&AnnotateWorkspacePointerIntent::alt_key>("alt_key"),
            api::field<&AnnotateWorkspacePointerIntent::meta_key>("meta_key"),
            api::field<&AnnotateWorkspacePointerIntent::drag_kind>("drag_kind"),
            api::field<&AnnotateWorkspacePointerIntent::object_index>("object_index"),
            api::field<&AnnotateWorkspacePointerIntent::handle>("handle"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.pointer", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspacePointerIntent");
    }
};

struct AnnotateWorkspaceFillIntent {
    int capture_x = 0;
    int capture_y = 0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceFillIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspaceFillIntent::capture_y>("capture_y", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.fill", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspacePointIntent");
    }
};

struct AnnotateWorkspaceColorSampleIntent {
    int capture_x = 0;
    int capture_y = 0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&AnnotateWorkspaceColorSampleIntent::capture_x>("capture_x", api::required),
            api::field<&AnnotateWorkspaceColorSampleIntent::capture_y>("capture_y", api::required),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("annotate.workspace.color_sample", api::workflow_set(Workflow::Annotate),
                           "AnnotateWorkspacePointIntent");
    }
};

struct ToolSelectIntentPayload {
    std::string tool;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ToolSelectIntentPayload::tool>("tool", api::required),
        };
    }

    [[nodiscard]] static constexpr bool api_additional_properties() {
        return true;
    }
};

struct ToolSelectIntent {
    std::string tool;
    nlohmann::json options = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ToolSelectIntent::tool>("tool", api::required),
            api::field<&ToolSelectIntent::options>("options", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("tool.select", api::workflow_set(Workflow::Predict, Workflow::Annotate, Workflow::Live),
                           "ToolSelectIntent");
    }
};

struct LivePreviewControlIntentPayload {
    bool enabled = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&LivePreviewControlIntentPayload::enabled>("enabled", api::required),
        };
    }
};

enum class LivePreviewControlTarget : std::uint8_t {
    FitToCapture = 0,
    CropOverlayMode = 1,
};

struct LivePreviewControlIntent {
    struct TargetForIntent {
        std::string_view id;
        LivePreviewControlTarget target;
    };

    std::optional<bool> fit_to_capture;
    std::optional<bool> crop_overlay_mode;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&LivePreviewControlIntent::fit_to_capture>("fit_to_capture"),
            api::field<&LivePreviewControlIntent::crop_overlay_mode>("crop_overlay_mode"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("live.preview.fit_to_capture", api::workflow_set(Workflow::Live),
                           "LivePreviewControlIntent");
    }

    [[nodiscard]] static constexpr auto api_intents() {
        return std::tuple{
            api::intent("live.preview.fit_to_capture", api::workflow_set(Workflow::Live), "LivePreviewControlIntent"),
            api::intent("live.preview.full_frame_display", api::workflow_set(Workflow::Live),
                        "LivePreviewControlIntent"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent_targets() {
        return std::array{
            TargetForIntent{"live.preview.fit_to_capture", LivePreviewControlTarget::FitToCapture},
            TargetForIntent{"live.preview.full_frame_display", LivePreviewControlTarget::CropOverlayMode},
        };
    }
};

struct UiLogIntent {
    std::string level = "info";
    std::string event;
    nlohmann::json fields = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&UiLogIntent::level>("level", api::required),
            api::field<&UiLogIntent::event>("event", api::required),
            api::field<&UiLogIntent::fields>("fields", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("ui.log",
                           api::workflow_set(Workflow::Train, Workflow::Validate, Workflow::Predict, Workflow::Annotate,
                                             Workflow::Export, Workflow::Live),
                           "UiLogIntent");
    }
};

struct WorkspaceSurfaceRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&WorkspaceSurfaceRect::x>("x", api::required),
            api::field<&WorkspaceSurfaceRect::y>("y", api::required),
            api::field<&WorkspaceSurfaceRect::width>("width", api::required),
            api::field<&WorkspaceSurfaceRect::height>("height", api::required),
        };
    }
};

struct WorkspaceBoundsIntent {
    CaptureRegion canvas_device{};
    WorkspaceSurfaceRect canvas_css{};
    CaptureRegion viewport_device{};
    WorkspaceSurfaceRect viewport_css{};
    double device_pixel_ratio = 1.0;
    std::optional<CaptureRegion> clip;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&WorkspaceBoundsIntent::canvas_device>("canvas_device", api::required),
            api::field<&WorkspaceBoundsIntent::canvas_css>("canvas_css", api::required),
            api::field<&WorkspaceBoundsIntent::viewport_device>("viewport_device", api::required),
            api::field<&WorkspaceBoundsIntent::viewport_css>("viewport_css", api::required),
            api::field<&WorkspaceBoundsIntent::device_pixel_ratio>("device_pixel_ratio", api::required,
                                                                   api::minimum(0.0)),
            api::field<&WorkspaceBoundsIntent::clip>("clip"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("workspace.bounds", api::workflow_set(Workflow::Annotate, Workflow::Live),
                           "WorkspaceBoundsIntent");
    }
};

struct SettingsUpdateIntent {
    nlohmann::json patch = nlohmann::json::object();

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&SettingsUpdateIntent::patch>("patch", api::additional_properties(true)),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("settings.update",
                           api::workflow_set(Workflow::Train, Workflow::Validate, Workflow::Predict, Workflow::Annotate,
                                             Workflow::Export, Workflow::Live),
                           "SettingsUpdateIntent");
    }
};

struct CropCommitIntent {
    bool has_crop = false;
    CaptureRegion crop{};

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&CropCommitIntent::has_crop>("has_crop"),
            api::field<&CropCommitIntent::crop>("crop"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("crop.commit", api::workflow_set(Workflow::Predict, Workflow::Annotate, Workflow::Live),
                           "CropCommitIntent");
    }
};

struct FileDialogFilter {
    std::string name;
    std::vector<std::string> patterns;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&FileDialogFilter::name>("name"),
            api::field<&FileDialogFilter::patterns>("patterns"),
        };
    }
};

struct FileDialogRequestIntent {
    std::uint64_t token = 0U;
    std::string dialog_id;
    std::string target_field;
    FileDialogMode mode = FileDialogMode::OpenFile;
    std::string title;
    std::vector<FileDialogFilter> filters;
    bool defer_apply = false;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&FileDialogRequestIntent::token>("token", api::required),
            api::field<&FileDialogRequestIntent::dialog_id>("dialog_id", api::required),
            api::field<&FileDialogRequestIntent::target_field>("target_field", api::required),
            api::field<&FileDialogRequestIntent::mode>("mode", api::required),
            api::field<&FileDialogRequestIntent::title>("title"),
            api::field<&FileDialogRequestIntent::filters>("filters"),
            api::field<&FileDialogRequestIntent::defer_apply>("defer_apply"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("file_dialog.request",
                           api::workflow_set(Workflow::Train, Workflow::Validate, Workflow::Predict, Workflow::Annotate,
                                             Workflow::Export, Workflow::Live),
                           "FileDialogRequestIntent");
    }
};

struct ViewportCommitIntent {
    double center_x = 0.0;
    double center_y = 0.0;
    double zoom = 1.0;
    std::optional<CaptureRegion> clip;

    [[nodiscard]] static constexpr auto api_fields() {
        return std::tuple{
            api::field<&ViewportCommitIntent::center_x>("center_x"),
            api::field<&ViewportCommitIntent::center_y>("center_y"),
            api::field<&ViewportCommitIntent::zoom>("zoom", api::minimum(0.0)),
            api::field<&ViewportCommitIntent::clip>("clip"),
        };
    }

    [[nodiscard]] static constexpr auto api_intent() {
        return api::intent("viewport.commit", api::workflow_set(Workflow::Predict, Workflow::Annotate, Workflow::Live),
                           "ViewportCommitIntent");
    }
};

using BrowserIntentPayloadTypes =
    api::type_list<TrainStartIntent, TrainStopIntent, TrainRemoteQueryIntent, TrainLocalGpuRefreshIntent,
                   DatasetCompileStartIntent, DatasetCompileCancelIntent, CustomModelSelectIntent, SettingsResetIntent,
                   TrainRemoteOfferArmIntent, TrainRemoteOfferClearIntent, TrainRemoteStartIntent,
                   TrainRemoteStopIntent, PredictStartIntent, PredictStopIntent, ValidateStartIntent, ExportStartIntent,
                   AnnotateSaveIntent, AnnotateSetupIntent, AnnotateHoldSaveIntent, AnnotateBrushRadiusIntent,
                   AnnotateSidebarIntent, AnnotateWorkspaceClickIntent, AnnotateWorkspaceBoxDragIntent,
                   AnnotateWorkspaceHandleDragIntent, AnnotateWorkspaceBrushIntent, AnnotateWorkspacePointerIntent,
                   AnnotateWorkspaceFillIntent, AnnotateWorkspaceColorSampleIntent, ToolSelectIntent,
                   LivePreviewControlIntent, SettingsUpdateIntent, UiLogIntent, WorkspaceBoundsIntent, CropCommitIntent,
                   FileDialogRequestIntent, ViewportCommitIntent>;

using BrowserBoundarySchemaTypes = api::type_list<
    Workflow, SourceKind, BrowserHostBackend, BrowserRuntimeCapabilityStatus, BrowserBridgePhase,
    ArtifactOperationPhase, WeightArtifactStatus, FileDialogMode, FileDialogResultStatus, CustomModelArtifactKind,
    ModelPreflightStatus, AnnotateWorkspacePhase, AnnotateHandleRole, AnnotateBoxDragKind, AnnotateSetupAction,
    CaptureRegion, JobLogEntry, JobState, SourceMetadata, AnnotationDocumentState, WorkspaceSurfaceInfo,
    BrowserRuntimeCapabilities, ArtifactSplitState, DatasetArtifactState, WeightArtifactState, ArtifactState,
    FileDialogState, ModelPreflightState, StateSnapshot, IntentMessage, TrainStartIntent, TrainStopIntent,
    TrainRemoteQueryIntent, TrainLocalGpuRefreshIntent, DatasetCompileStartIntent, DatasetCompileCancelIntent,
    CustomModelSelectIntent, SettingsResetIntent, TrainRemoteOfferArmIntent, TrainRemoteOfferClearIntent,
    TrainRemoteStartIntent, TrainRemoteStopIntent, PredictStartIntentPayload, PredictStartIntent, EmptyIntentPayload,
    PredictStopIntent, ValidateStartIntent, ExportStartIntent, AnnotateSaveIntent, AnnotateLiveStartIntentPayload,
    AnnotateLiveStopIntentPayload, AnnotateSetupIntent, AnnotateHoldSaveIntent, AnnotateBrushRadiusIntent,
    AnnotateSidebarIntent, AnnotateWorkspaceHandleRef, AnnotateWorkspaceClickIntent, AnnotateWorkspaceBoxDragIntent,
    AnnotateWorkspaceHandleDragIntent, AnnotateWorkspaceBrushIntent, AnnotateWorkspacePointerIntent,
    AnnotateWorkspaceFillIntent, AnnotateWorkspaceColorSampleIntent, ToolSelectIntentPayload, ToolSelectIntent,
    LivePreviewControlIntentPayload, LivePreviewControlIntent, UiLogIntent, WorkspaceSurfaceRect, WorkspaceBoundsIntent,
    SettingsUpdateIntent, CropCommitIntent, FileDialogFilter, FileDialogRequestIntent, ViewportCommitIntent>;

using RoutedIntentPayload = api::factory<BrowserIntentPayloadTypes>::payload_variant;
inline constexpr std::size_t kRoutedIntentPayloadAlternativeCount = std::variant_size_v<RoutedIntentPayload>;
static_assert(api::intent_metadata<BrowserIntentPayloadTypes>::valid(),
              "browser intent metadata contains duplicate ids or invalid workflows");
static_assert(api::type_list_metadata<BrowserBoundarySchemaTypes>::unique_types(),
              "browser boundary schema type list contains duplicates");

struct RoutedIntent {
    std::uint32_t protocol_version = kHostApiProtocolVersion;
    std::uint64_t request_id = 0;
    Workflow workflow = Workflow::Train;
    std::string intent;
    RoutedIntentPayload payload;
};

[[nodiscard]] std::string_view routed_intent_name(const RoutedIntentPayload& payload) noexcept;
[[nodiscard]] std::string_view routed_intent_name(const RoutedIntent& routed) noexcept;
[[nodiscard]] RoutedIntent route_intent(const IntentMessage& intent);

void to_json(nlohmann::json& j, const IntentMessage& intent);
void from_json(const nlohmann::json& j, IntentMessage& intent);

}  
