#include "gui/browser_host_adapters.h"

#include "browser/host_api_dispatch.h"
#include "gui/annotation/common.h"
#include "gui/annotation/controller.h"
#include "gui/annotation/document/document.h"
#include "gui/annotation/document/types.h"
#include "gui/annotation/editor.h"
#include "gui/annotation/editor_history.h"
#include "gui/annotation/render/renderer.h"
#include "gui/annotation/session.h"
#include "gui/annotation_core.h"
#include "gui/gui_settings.h"
#include "gui/source_selection.h"
#include "gui/train_command.h"
#include "gui/view_state.h"
#include "mmltk/live/live_types.h"
#include "mmltk/live/manual_overlay_document.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mmltk::browser {

namespace {

constexpr int kCreateCommitMinExtent = 6;

void set_annotation_brush_preview(mmltk::gui::AnnotationSession& session, const int capture_x, const int capture_y,
                                  const int radius, const bool erase) {
    session.set_brush_preview(mmltk::gui::AnnotationBrushPreview{
        true,
        capture_x,
        capture_y,
        std::max(1, radius),
        erase,
    });
}

void apply_native_host_publication_metadata(FrameSlotDescriptor& descriptor) {
    descriptor.lifecycle = FrameSlotLifecycle::ExplicitRelease;
    descriptor.ownership = FrameSlotOwnership::NativeHost;

    LinuxImportableFrameSlotContract contract;
    contract.ready_sync = FrameSlotReadySync{
        FrameSlotSyncKind::TimelinePoint,
        std::string{},
        descriptor.ready_ns,
    };
    contract.metadata = FrameSlotNativeImportMetadata{};
    contract.metadata->release_behavior = FrameSlotNativeImportReleaseBehavior::ContractRelease;
    apply_linux_importable_frame_slot_contract(descriptor, std::move(contract));
}

template <typename Dimensions>
FrameSlotDescriptor make_frame_slot_descriptor(std::string slot_name, FrameTransportKind transport,
                                               FramePixelFormat pixel_format, const std::uint32_t slot_index,
                                               const mmltk::live::LiveFrameId& frame_id, const Dimensions& dims,
                                               const mmltk::live::LiveCaptureRegion& region,
                                               const std::uint64_t ready_ns, const bool short_frame) {
    FrameSlotDescriptor descriptor;
    descriptor.slot_name = std::move(slot_name);
    descriptor.transport = transport;
    descriptor.pixel_format = pixel_format;
    descriptor.slot_index = slot_index;
    descriptor.frame_id = frame_id;
    descriptor.capture_region = capture_region_from_live_region(region);
    descriptor.width = dims.width;
    descriptor.height = dims.height;
    descriptor.row_stride_bytes = dims.pitch_bytes;
    descriptor.byte_length = frame_byte_length(descriptor.row_stride_bytes, descriptor.height);
    descriptor.ready_ns = ready_ns;
    descriptor.short_frame = short_frame;
    if (descriptor.transport != FrameTransportKind::CudaDeviceBuffer) {
        throw std::runtime_error(
            "browser frame publication requires "
            "FrameTransportKind::CudaDeviceBuffer");
    }
    apply_native_host_publication_metadata(descriptor);
    if (!frame_slot_native_import_metadata(descriptor).has_value()) {
        throw std::runtime_error(
            "native-host published frame slots must preserve native import "
            "metadata");
    }
    return descriptor;
}

std::optional<SourceMetadata> infer_source_metadata(const mmltk::gui::GuiSettingsSnapshot& gui) {
    switch (gui.current_view) {
        case mmltk::gui::View::Predict:
        case mmltk::gui::View::Live:
            if (gui.workflows.predict != nullptr) {
                return source_metadata_from_selection(gui.workflows.predict->source);
            }
            break;
        case mmltk::gui::View::Annotate:
            if (gui.workflows.annotate != nullptr) {
                return source_metadata_from_selection(gui.workflows.annotate->source);
            }
            break;
        case mmltk::gui::View::Train:
        case mmltk::gui::View::Validate:
        case mmltk::gui::View::Export:
            break;
    }
    return std::nullopt;
}

int saturating_int(const std::uint32_t value) noexcept {
    constexpr auto kMaxInt = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    return static_cast<int>(value > kMaxInt ? kMaxInt : value);
}

[[nodiscard]] std::optional<mmltk::gui::View> view_from_name(const std::string_view name) noexcept {
    constexpr std::array<BrowserNamedValue<mmltk::gui::View>, 6> kViewNames{{
        {"train", mmltk::gui::View::Train},
        {"validate", mmltk::gui::View::Validate},
        {"predict", mmltk::gui::View::Predict},
        {"annotate", mmltk::gui::View::Annotate},
        {"export", mmltk::gui::View::Export},
        {"live", mmltk::gui::View::Live},
    }};
    return find_browser_named_value(name, kViewNames);
}

[[nodiscard]] nlohmann::json normalize_settings_patch(nlohmann::json patch) {
    const auto view_it = patch.find("current_view");
    if (view_it == patch.end() || !view_it->is_string()) {
        return patch;
    }

    const std::optional<mmltk::gui::View> view = view_from_name(view_it->get_ref<const std::string&>());
    if (!view.has_value()) {
        throw std::runtime_error("unsupported browser settings.update current_view: " + view_it->get<std::string>());
    }
    *view_it = static_cast<int>(*view);
    return patch;
}

void refresh_annotation_pointer_capture(mmltk::gui::AnnotationSession& session) {
    const std::optional<mmltk::gui::AnnotationHandleDragState> handle_drag = session.handle_drag();
    const bool handle_drag_active = handle_drag.has_value() && handle_drag->active;
    session.set_pointer_captured(session.create_drag_session().active || session.crop_drag_session().active ||
                                 session.direct_drag_session().active || handle_drag_active ||
                                 session.paint_stroke().active);
}

[[nodiscard]] const mmltk::gui::AnnotationObject* selected_mask_editable_object(
    const mmltk::gui::AnnotationDocument& document, const mmltk::gui::AnnotationSession& session) noexcept {
    const std::optional<std::size_t> selected_index = mmltk::gui::normalize_selected_object_index(document, session);
    if (!selected_index.has_value()) {
        return nullptr;
    }

    const mmltk::gui::AnnotationObject* object = document.object(*selected_index);
    return object != nullptr && mmltk::gui::annotation_object_supports_mask_editing(*object) ? object : nullptr;
}

[[nodiscard]] std::optional<std::size_t> selected_object_index(const mmltk::gui::AnnotationDocument& document,
                                                               const mmltk::gui::AnnotationSession& session) noexcept {
    return mmltk::gui::normalize_selected_object_index(document, session);
}

[[nodiscard]] mmltk::gui::AnnotationHandleRole annotation_handle_role_from_browser_role(
    const AnnotateHandleRole role) noexcept {
    using GuiRole = mmltk::gui::AnnotationHandleRole;
    switch (role) {
        case AnnotateHandleRole::Point:
            return GuiRole::Point;
        case AnnotateHandleRole::SplineKnot:
            return GuiRole::SplineKnot;
        case AnnotateHandleRole::SplineInHandle:
            return GuiRole::SplineInHandle;
        case AnnotateHandleRole::SplineOutHandle:
            return GuiRole::SplineOutHandle;
        case AnnotateHandleRole::SkeletonNode:
            return GuiRole::SkeletonNode;
    }
    return GuiRole::None;
}

[[nodiscard]] mmltk::gui::AnnotationHandleId annotation_handle_id_from_browser_ref(
    const AnnotateWorkspaceHandleRef& handle) noexcept {
    return mmltk::gui::AnnotationHandleId{
        static_cast<std::size_t>(handle.object_index),
        static_cast<std::size_t>(handle.element_index),
        annotation_handle_role_from_browser_role(handle.role),
    };
}

[[nodiscard]] bool annotation_handle_exists(const mmltk::gui::AnnotationDocument& document,
                                            const mmltk::gui::AnnotationHandleId& handle) {
    const mmltk::gui::AnnotationObject* object = document.object(handle.object_index);
    if (object == nullptr) {
        return false;
    }

    return std::visit(
        [&handle](const auto& shape) -> bool {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, mmltk::gui::AnnotationPointShape>) {
                return handle.role == mmltk::gui::AnnotationHandleRole::Point && handle.element_index == 0U;
            } else if constexpr (std::is_same_v<Shape, mmltk::gui::AnnotationSplineShape>) {
                if (handle.element_index >= shape.knots.size()) {
                    return false;
                }
                return handle.role == mmltk::gui::AnnotationHandleRole::SplineKnot ||
                       handle.role == mmltk::gui::AnnotationHandleRole::SplineInHandle ||
                       handle.role == mmltk::gui::AnnotationHandleRole::SplineOutHandle;
            } else if constexpr (std::is_same_v<Shape, mmltk::gui::AnnotationSkeletonShape>) {
                return handle.role == mmltk::gui::AnnotationHandleRole::SkeletonNode &&
                       handle.element_index < shape.nodes.size();
            } else {
                return false;
            }
        },
        object->shape);
}

void require_annotate_workspace_context(const BrowserAnnotateWorkspaceContext& context,
                                        const std::string_view intent_name) {
    if (context.document == nullptr || context.session == nullptr || context.controller == nullptr ||
        context.frame == nullptr || context.categories == nullptr) {
        throw std::runtime_error("browser " + std::string(intent_name) +
                                 " requires bound annotation document, session, controller, "
                                 "frame, and categories");
    }
}

void require_annotate_workspace_document_context(const BrowserAnnotateWorkspaceContext& context,
                                                 const std::string_view intent_name) {
    if (context.document == nullptr || context.session == nullptr || context.frame == nullptr) {
        throw std::runtime_error("browser " + std::string(intent_name) +
                                 " requires bound annotation document, session, and frame");
    }
}

bool cancel_annotate_workspace_interaction(const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.cancel");
    if (context.document->transaction_active()) {
        context.document->cancel_transaction();
    }
    context.session->clear_transient_state();
    refresh_annotation_pointer_capture(*context.session);
    return true;
}

[[nodiscard]] mmltk::gui::RectDragKind annotation_box_drag_kind_from_browser_kind(
    const AnnotateBoxDragKind drag_kind) noexcept {
    using GuiKind = mmltk::gui::RectDragKind;
    switch (drag_kind) {
        case AnnotateBoxDragKind::Create:
            return GuiKind::Create;
        case AnnotateBoxDragKind::Move:
            return GuiKind::Move;
        case AnnotateBoxDragKind::ResizeTopLeft:
            return GuiKind::ResizeTopLeft;
        case AnnotateBoxDragKind::ResizeTopRight:
            return GuiKind::ResizeTopRight;
        case AnnotateBoxDragKind::ResizeBottomLeft:
            return GuiKind::ResizeBottomLeft;
        case AnnotateBoxDragKind::ResizeBottomRight:
            return GuiKind::ResizeBottomRight;
    }
    return GuiKind::None;
}

[[nodiscard]] std::optional<AnnotateBoxDragKind> browser_box_drag_kind_from_session(
    const mmltk::gui::PreviewRectDragSession& session) noexcept {
    using GuiKind = mmltk::gui::RectDragKind;
    switch (session.drag.kind) {
        case GuiKind::Create:
            return AnnotateBoxDragKind::Create;
        case GuiKind::Move:
            return AnnotateBoxDragKind::Move;
        case GuiKind::ResizeTopLeft:
            return AnnotateBoxDragKind::ResizeTopLeft;
        case GuiKind::ResizeTopRight:
            return AnnotateBoxDragKind::ResizeTopRight;
        case GuiKind::ResizeBottomLeft:
            return AnnotateBoxDragKind::ResizeBottomLeft;
        case GuiKind::ResizeBottomRight:
            return AnnotateBoxDragKind::ResizeBottomRight;
        case GuiKind::None:
            break;
    }
    return std::nullopt;
}

[[nodiscard]] mmltk::gui::CanvasViewport annotation_capture_identity_viewport(
    const mmltk::gui::AnnotationFrame& frame) {
    const std::uint32_t capture_width = mmltk::gui::annotation_frame_capture_width(frame);
    const std::uint32_t capture_height = mmltk::gui::annotation_frame_capture_height(frame);
    return mmltk::gui::make_canvas_viewport(0.0F, 0.0F, static_cast<float>(capture_width),
                                            static_cast<float>(capture_height), capture_width, capture_height);
}

[[nodiscard]] std::size_t default_box_create_category_index(const BrowserAnnotateWorkspaceContext& context) {
    if (context.categories != nullptr) {
        (void)mmltk::gui::AnnotationEditor::ensure_default_category(*context.categories);
    }

    const std::optional<std::size_t> selected_index = selected_object_index(*context.document, *context.session);
    const mmltk::gui::AnnotationObject* selected_object =
        selected_index.has_value() ? context.document->object(*selected_index) : nullptr;
    std::size_t category_index = selected_object != nullptr ? selected_object->category_index : 0U;
    if (context.categories != nullptr && !context.categories->items.empty()) {
        category_index = std::min(category_index, context.categories->items.size() - 1U);
    }
    return category_index;
}

[[nodiscard]] mmltk::gui::SourceSelectionState* source_selection_for_workflow(
    const Workflow workflow, const BrowserHostIntentContext& context) noexcept {
    switch (workflow) {
        case Workflow::Predict:
        case Workflow::Live:
            return context.predict != nullptr ? &context.predict->source : nullptr;
        case Workflow::Annotate:
            return context.annotate != nullptr ? &context.annotate->source : nullptr;
        case Workflow::Train:
        case Workflow::Validate:
        case Workflow::Export:
            break;
    }
    return nullptr;
}

[[nodiscard]] mmltk::gui::AnnotationToolKind annotation_tool_kind_from_name_impl(const std::string_view name) {
    using Tool = mmltk::gui::AnnotationToolKind;
    constexpr std::array<BrowserNamedValue<Tool>, 15> kToolNames{{
        {"select", Tool::Select},
        {"direct", Tool::Direct},
        {"box", Tool::Box},
        {"paint", Tool::MaskPaint},
        {"mask.paint", Tool::MaskPaint},
        {"mask_paint", Tool::MaskPaint},
        {"erase", Tool::MaskErase},
        {"mask.erase", Tool::MaskErase},
        {"mask_erase", Tool::MaskErase},
        {"fill", Tool::MaskFill},
        {"mask.fill", Tool::MaskFill},
        {"mask_fill", Tool::MaskFill},
        {"spline", Tool::Spline},
        {"point", Tool::Point},
        {"skeleton", Tool::Skeleton},
    }};
    if (const std::optional<Tool> tool = find_browser_named_value(name, kToolNames); tool.has_value()) {
        return *tool;
    }
    throw std::runtime_error("unsupported browser tool.select tool: " + std::string(name));
}

[[nodiscard]] nlohmann::json make_workflow_intent_settings_patch(const std::string_view workflow_key,
                                                                 const std::string_view workflow_state_key,
                                                                 const nlohmann::json& options,
                                                                 const std::optional<std::string>& selected_preset) {
    nlohmann::json patch = nlohmann::json::object();
    if (selected_preset.has_value()) {
        patch["selected_preset"] = *selected_preset;
    }
    if (!options.is_object() || options.empty()) {
        return patch;
    }

    if (const auto current_view = options.find("current_view"); current_view != options.end()) {
        patch["current_view"] = *current_view;
    }
    if (const auto ui = options.find("ui"); ui != options.end() && ui->is_object()) {
        patch["ui"] = *ui;
    }

    nlohmann::json workflow_patch = nlohmann::json::object();
    if (const auto workflows = options.find("workflows"); workflows != options.end() && workflows->is_object()) {
        patch["workflows"] = *workflows;
        if (const auto workflow_it = workflows->find(std::string(workflow_key));
            workflow_it != workflows->end() && workflow_it->is_object()) {
            workflow_patch = *workflow_it;
        }
    }

    constexpr std::array<std::string_view, 4> kKnownWorkflowKeys{
        "source",
        "model_artifacts",
        "execution",
        "predict",
    };
    constexpr std::array<std::string_view, 4> kKnownAnnotateKeys{
        "source",
        "model_artifacts",
        "execution",
        "annotate",
    };
    const auto& known_keys = workflow_state_key == "annotate" ? kKnownAnnotateKeys : kKnownWorkflowKeys;

    nlohmann::json leaf_patch = nlohmann::json::object();
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (!it.key().empty() && (it.key() == "selected_preset" || it.key() == "current_view" || it.key() == "ui" ||
                                  it.key() == "workflows")) {
            continue;
        }

        bool known_key = false;
        for (const std::string_view key : known_keys) {
            if (it.key() == key) {
                workflow_patch[it.key()] = *it;
                known_key = true;
                break;
            }
        }
        if (!known_key) {
            leaf_patch[it.key()] = *it;
        }
    }

    if (!leaf_patch.empty()) {
        nlohmann::json& workflow_state_patch = workflow_patch[std::string(workflow_state_key)];
        if (!workflow_state_patch.is_object()) {
            workflow_state_patch = nlohmann::json::object();
        }
        workflow_state_patch.merge_patch(leaf_patch);
    }

    if (!workflow_patch.empty()) {
        patch["workflows"][std::string(workflow_key)] = std::move(workflow_patch);
    }
    return patch;
}

std::string format_memory_gib(const std::uint64_t bytes) {
    constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;
    return std::format("{:.1f} GiB", static_cast<double>(bytes) / kBytesPerGiB);
}

std::string local_gpu_label(const mmltk::gui::LocalGpuInfo& gpu) {
    std::string label = std::format("GPU {} · {}", gpu.device_id, gpu.name);
    if (gpu.total_memory_bytes > 0) {
        label += std::format(" · {}", format_memory_gib(gpu.total_memory_bytes));
    }
    return label;
}

std::size_t count_selected_gpus(std::span<const mmltk::gui::LocalGpuInfo> gpus,
                                const std::vector<bool>& selected) noexcept {
    std::size_t selected_count = 0;
    for (std::size_t index = 0; index < std::min(gpus.size(), selected.size()); ++index) {
        if (selected[index]) {
            ++selected_count;
        }
    }
    return selected_count;
}

std::optional<BrowserBannerState> train_local_gpu_banner(std::span<const mmltk::gui::LocalGpuInfo> gpus,
                                                         const std::vector<bool>& selected,
                                                         const std::string_view error) {
    if (!error.empty()) {
        return BrowserBannerState{
            "error",
            std::string(error),
        };
    }
    if (gpus.empty()) {
        return BrowserBannerState{
            "missing_inventory",
            "No visible CUDA GPUs were found for local training.",
        };
    }
    if (count_selected_gpus(gpus, selected) == 0U) {
        return BrowserBannerState{
            "selection_required",
            "Select at least one local GPU before running training.",
        };
    }
    return std::nullopt;
}

[[nodiscard]] nlohmann::json live_frame_id_json(const mmltk::live::LiveFrameId& frame_id) {
    return nlohmann::json{
        {"session_nonce", frame_id.session_nonce},
        {"sequence", frame_id.sequence},
    };
}

[[nodiscard]] nlohmann::json optional_live_frame_id_json(const std::optional<mmltk::live::LiveFrameId>& frame_id) {
    return frame_id.has_value() ? live_frame_id_json(*frame_id) : nlohmann::json(nullptr);
}

}  // namespace

mmltk::gui::AnnotationToolKind annotation_tool_kind_from_name(const std::string_view name) {
    return annotation_tool_kind_from_name_impl(name);
}

Workflow workflow_from_view(const mmltk::gui::View view) noexcept {
    switch (view) {
        case mmltk::gui::View::Train:
            return Workflow::Train;
        case mmltk::gui::View::Validate:
            return Workflow::Validate;
        case mmltk::gui::View::Predict:
            return Workflow::Predict;
        case mmltk::gui::View::Annotate:
            return Workflow::Annotate;
        case mmltk::gui::View::Export:
            return Workflow::Export;
        case mmltk::gui::View::Live:
            return Workflow::Live;
    }
    return Workflow::Train;
}

CaptureRegion capture_region_from_live_region(const mmltk::live::LiveCaptureRegion& region) noexcept {
    return CaptureRegion{
        region.x,
        region.y,
        region.width,
        region.height,
    };
}

SourceMetadata source_metadata_from_selection(const mmltk::gui::SourceSelectionState& source) {
    SourceMetadata metadata;
    metadata.recursive = source.recursive;
    metadata.device_index = source.device_index;
    metadata.capture_width = source.capture_width;
    metadata.capture_height = source.capture_height;
    metadata.capture_fps = source.capture_fps;
    metadata.v4l2_buffer_count = source.v4l2_buffer_count;
    metadata.has_crop = source.crop_width > 0 && source.crop_height > 0;
    metadata.crop = CaptureRegion{
        static_cast<std::uint32_t>(source.crop_x < 0 ? 0 : source.crop_x),
        static_cast<std::uint32_t>(source.crop_y < 0 ? 0 : source.crop_y),
        static_cast<std::uint32_t>(source.crop_width < 0 ? 0 : source.crop_width),
        static_cast<std::uint32_t>(source.crop_height < 0 ? 0 : source.crop_height),
    };

    switch (source.kind) {
        case mmltk::gui::SourceKind::CompiledDataset:
            metadata.kind = SourceKind::CompiledDataset;
            metadata.locator = source.compiled_path;
            break;
        case mmltk::gui::SourceKind::SingleImage:
            metadata.kind = SourceKind::SingleImage;
            metadata.locator = source.single_image_path;
            break;
        case mmltk::gui::SourceKind::ImageFolder:
            metadata.kind = SourceKind::ImageFolder;
            metadata.locator = source.image_directory;
            break;
        case mmltk::gui::SourceKind::VideoStream:
            metadata.kind = SourceKind::VideoStream;
            metadata.locator = "device:" + std::to_string(source.device_index);
            break;
    }

    return metadata;
}

AnnotationDocumentState annotation_document_state_from_snapshot(
    const mmltk::live::ManualOverlayDocumentSnapshot& snapshot) {
    AnnotationDocumentState state;
    state.document_generation = snapshot.document_generation;
    state.session_revision = snapshot.session_revision;
    state.capture_width = snapshot.capture_width;
    state.capture_height = snapshot.capture_height;
    state.instance_count = snapshot.instances.size();
    if (snapshot.selected_instance.has_value()) {
        state.selected_instance = static_cast<std::uint32_t>(*snapshot.selected_instance);
    }
    return state;
}

std::optional<AnnotationDocumentState> annotation_document_state_from_editor(
    const mmltk::gui::AnnotationFrame* frame, const mmltk::gui::AnnotationDocument* document,
    const mmltk::gui::AnnotationSession* session) {
    if (frame == nullptr || document == nullptr || session == nullptr) {
        return std::nullopt;
    }
    return annotation_document_state_from_snapshot(
        mmltk::gui::AnnotationRenderer::build_manual_overlay_snapshot(*frame, *document, *session));
}

nlohmann::json train_local_gpu_state_to_json(const std::span<const mmltk::gui::LocalGpuInfo> gpus,
                                             const std::vector<bool>& selected,
                                             const std::span<const int> configured_device_ids,
                                             const std::string_view error, const bool refresh_running) {
    nlohmann::json devices = nlohmann::json::array();
    nlohmann::json configured_ids_json = nlohmann::json::array();
    for (const int device_id : configured_device_ids) {
        configured_ids_json.push_back(device_id);
    }
    std::vector<int> selected_device_ids;
    selected_device_ids.reserve(gpus.size());
    for (std::size_t index = 0; index < gpus.size(); ++index) {
        const bool device_selected = index < selected.size() ? selected[index] : false;
        if (device_selected) {
            selected_device_ids.push_back(gpus[index].device_id);
        }
        devices.push_back(nlohmann::json{
            {"device_id", gpus[index].device_id},
            {"name", gpus[index].name},
            {"label", local_gpu_label(gpus[index])},
            {"total_memory_bytes", gpus[index].total_memory_bytes},
            {"selected", device_selected},
        });
    }

    const std::size_t selected_count = selected_device_ids.size();
    std::string selection_summary;
    if (gpus.empty()) {
        selection_summary = "No visible GPUs";
    } else if (selected_count == 0U) {
        selection_summary = "No GPUs selected";
    } else if (selected_count == gpus.size()) {
        selection_summary = std::format("{} visible GPUs selected", selected_count);
    } else {
        selection_summary = std::format("{} / {} GPUs selected", selected_count, gpus.size());
    }

    nlohmann::json state = {
        {"refresh_running", refresh_running},
        {"refresh_action_label", refresh_running ? "Refreshing GPUs..." : "Refresh Visible GPUs"},
        {"selection_summary", std::move(selection_summary)},
        {"visible_count", gpus.size()},
        {"selected_count", selected_count},
        {"has_visible_gpus", !gpus.empty()},
        {"has_selection", selected_count > 0U},
        {"configured_device_ids", std::move(configured_ids_json)},
        {"selected_device_ids", std::move(selected_device_ids)},
        {"error", std::string(error)},
        {"devices", std::move(devices)},
        {"banner", nlohmann::json(nullptr)},
    };

    if (const std::optional<BrowserBannerState> banner = train_local_gpu_banner(gpus, selected, error);
        banner.has_value()) {
        state["banner"] = nlohmann::json{
            {"kind", banner->kind},
            {"message", banner->message},
        };
    }

    return state;
}

nlohmann::json live_runtime_state_to_json(const BrowserLiveRuntimeState& state) {
    return nlohmann::json{
        {"active_mode", state.active_mode.empty() ? "none" : state.active_mode},
        {"startup_state", state.startup_state.empty() ? "idle" : state.startup_state},
        {"starting", state.starting},
        {"stopping", state.stopping},
        {"show_running_section", state.show_running_section},
        {"show_static_preview", state.show_static_preview},
        {"show_idle_start_error", state.show_idle_start_error},
        {"video_stream_source", state.video_stream_source},
        {"runtime_capabilities", state.runtime_capabilities},
        {"preview",
         {
             {"initialized", state.preview.initialized},
             {"has_frame", state.preview.has_frame},
             {"displayed_region", state.preview.displayed_region},
             {"frame_id", state.preview.frame_id},
             {"live_frame_id", optional_live_frame_id_json(state.preview.live_frame_id)},
             {"fit_to_capture", state.preview.fit_to_capture},
             {"crop_overlay_mode", state.preview.crop_overlay_mode},
             {"static_source_name", state.preview.static_source_name},
             {"error", state.preview.error},
             {"interop_failed", state.preview.interop_failed},
             {"last_failure_reason", state.preview.last_failure_reason},
             {"last_failure_detail", state.preview.last_failure_detail},
             {"last_failure_revision", state.preview.last_failure_revision},
             {"last_failure_stage", state.preview.last_failure_stage},
             {"last_failure_live_frame_id", optional_live_frame_id_json(state.preview.last_failure_live_frame_id)},
             {"last_failure_cuda_device",
              state.preview.last_failure_cuda_device.has_value()
                  ? nlohmann::json(*state.preview.last_failure_cuda_device)
                  : nlohmann::json(nullptr)},
             {"last_failure_result_code", state.preview.last_failure_result_code},
             {"last_failure_result_detail", state.preview.last_failure_result_detail},
             {"publication_counters",
              {
                  {"attempted_workspace_acquisitions",
                   state.preview.publication_counters.attempted_workspace_acquisitions},
                  {"startup_workspace_acquisition_misses",
                   state.preview.publication_counters.startup_workspace_acquisition_misses},
                  {"post_startup_workspace_acquisition_misses",
                   state.preview.publication_counters.post_startup_workspace_acquisition_misses},
                  {"retained_surfaces", state.preview.publication_counters.retained_surfaces},
                  {"rejected_stale_frames", state.preview.publication_counters.rejected_stale_frames},
                  {"cef_export_failures", state.preview.publication_counters.cef_export_failures},
                  {"renderer_releases", state.preview.publication_counters.renderer_releases},
                  {"native_release_failures", state.preview.publication_counters.native_release_failures},
                  {"renderer_import_failures", state.preview.publication_counters.renderer_import_failures},
                  {"renderer_release_rejections", state.preview.publication_counters.renderer_release_rejections},
              }},
         }},
        {"controller",
         {
             {"present", state.controller.present},
             {"running", state.controller.running},
             {"last_error", state.controller.last_error},
         }},
        {"fanout",
         {
             {"running", state.fanout.running},
             {"frames_fanned_out", state.fanout.frames_fanned_out},
             {"skipped_detect_publishes", state.fanout.skipped_detect_publishes},
             {"skipped_output_publishes", state.fanout.skipped_output_publishes},
             {"release_backlog", state.fanout.release_backlog},
             {"acquire_misses", state.fanout.acquire_misses},
             {"last_error", state.fanout.last_error},
         }},
        {"analyzer",
         {
             {"attached", state.analyzer.attached},
             {"model_hot", state.analyzer.model_hot},
             {"running", state.analyzer.running},
             {"frames_analyzed", state.analyzer.frames_analyzed},
             {"frames_skipped", state.analyzer.frames_skipped},
             {"last_latency_ms", state.analyzer.last_latency_ms},
             {"backend_name", state.analyzer.backend_name},
             {"last_error", state.analyzer.last_error},
         }},
        {"manual_overlay",
         {
             {"running", state.manual_overlay.running},
             {"generations_rendered", state.manual_overlay.generations_rendered},
             {"last_generation", state.manual_overlay.last_generation},
             {"last_error", state.manual_overlay.last_error},
         }},
        {"compositor",
         {
             {"running", state.compositor.running},
             {"frames_composited", state.compositor.frames_composited},
             {"frames_composited_after_startup", state.compositor.frames_composited_after_startup},
             {"frames_dropped", state.compositor.frames_dropped},
             {"skipped_compositor_presents", state.compositor.skipped_compositor_presents},
             {"front_slot_index", state.compositor.front_slot_index},
             {"front_slot_revision", state.compositor.front_slot_revision},
             {"last_frame_id", optional_live_frame_id_json(state.compositor.last_frame_id)},
             {"manual_overlay_active", state.compositor.manual_overlay_active},
             {"analysis_overlay_active", state.compositor.analysis_overlay_active},
             {"last_error", state.compositor.last_error},
         }},
        {"single_buffer_diagnostic",
         {
             {"enabled", state.single_buffer_diagnostic.enabled},
             {"frame_count", state.single_buffer_diagnostic.frame_count},
             {"frame_budget_ns", state.single_buffer_diagnostic.frame_budget_ns},
             {"drawn_frames", state.single_buffer_diagnostic.drawn_frames},
             {"consecutive_miss_limit", state.single_buffer_diagnostic.consecutive_miss_limit},
             {"completed", state.single_buffer_diagnostic.completed},
             {"analyzer_disabled", state.single_buffer_diagnostic.analyzer_disabled},
             {"failed", state.single_buffer_diagnostic.failed},
             {"failure_stage", state.single_buffer_diagnostic.failure_stage},
             {"failure_frame", state.single_buffer_diagnostic.failure_frame},
             {"failure_latency_us", state.single_buffer_diagnostic.failure_latency_us},
         }},
        {"start_error", state.start_error},
        {"action_error", state.action_error},
    };
}

const char* browser_live_display_startup_state(const bool capture_running, const std::string_view surface_revision,
                                               const std::string_view renderer_imported_revision,
                                               const std::string_view renderer_submitted_revision,
                                               const std::string_view renderer_drawn_revision) noexcept {
    if (surface_revision.empty()) {
        return capture_running ? "capture running" : "idle";
    }
    if (renderer_submitted_revision == surface_revision || renderer_drawn_revision == surface_revision) {
        return "draw submitted";
    }
    if (renderer_imported_revision == surface_revision) {
        return "texture imported";
    }
    return "surface published";
}

nlohmann::json make_predict_start_settings_patch(const PredictStartIntent& intent) {
    return make_workflow_intent_settings_patch("predict", "predict", intent.options, intent.selected_preset);
}

nlohmann::json make_annotate_save_settings_patch(const AnnotateSaveIntent& intent) {
    return make_workflow_intent_settings_patch("annotate", "annotate", intent.options, std::nullopt);
}

BrowserFileDialogTarget browser_file_dialog_target(const BrowserFileDialogBinding binding,
                                                   const BrowserFileDialogStateAccess& access) noexcept {
    switch (binding) {
        case BrowserFileDialogBinding::TrainTrainCompiledPath:
            return access.train != nullptr ? BrowserFileDialogTarget{&access.train->train_compiled_path}
                                           : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::TrainValCompiledPath:
            return access.train != nullptr ? BrowserFileDialogTarget{&access.train->val_compiled_path}
                                           : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::TrainTestCompiledPath:
            return access.train != nullptr ? BrowserFileDialogTarget{&access.train->test_compiled_path}
                                           : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::TrainWeights:
            return access.train != nullptr ? BrowserFileDialogTarget{&access.train->weights_path}
                                           : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::TrainResumePath:
            return access.train != nullptr ? BrowserFileDialogTarget{&access.train->resume_path}
                                           : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::TrainOutputDir:
            return access.train != nullptr ? BrowserFileDialogTarget{&access.train->output_dir}
                                           : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::PredictSingleImage:
            return access.predict != nullptr
                       ? BrowserFileDialogTarget{&access.predict->source.single_image_path, &access.predict->source,
                                                 mmltk::gui::SourceKind::SingleImage}
                       : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::PredictImageFolder:
            return access.predict != nullptr
                       ? BrowserFileDialogTarget{&access.predict->source.image_directory, &access.predict->source,
                                                 mmltk::gui::SourceKind::ImageFolder}
                       : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::PredictWeights:
            return access.predict != nullptr ? BrowserFileDialogTarget{&access.predict->weights_path}
                                             : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::PredictOnnx:
            return access.predict != nullptr ? BrowserFileDialogTarget{&access.predict->onnx_path}
                                             : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::PredictTensorRt:
            return access.predict != nullptr ? BrowserFileDialogTarget{&access.predict->tensorrt_path}
                                             : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::PredictOutputPath:
            return access.predict != nullptr ? BrowserFileDialogTarget{&access.predict->output_path}
                                             : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::AnnotateSingleImage:
            return access.annotate != nullptr
                       ? BrowserFileDialogTarget{&access.annotate->source.single_image_path, &access.annotate->source,
                                                 mmltk::gui::SourceKind::SingleImage}
                       : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::AnnotateImageFolder:
            return access.annotate != nullptr
                       ? BrowserFileDialogTarget{&access.annotate->source.image_directory, &access.annotate->source,
                                                 mmltk::gui::SourceKind::ImageFolder}
                       : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::AnnotateWeights:
            return access.annotate != nullptr ? BrowserFileDialogTarget{&access.annotate->weights_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::AnnotateOnnx:
            return access.annotate != nullptr ? BrowserFileDialogTarget{&access.annotate->onnx_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::AnnotateTensorRt:
            return access.annotate != nullptr ? BrowserFileDialogTarget{&access.annotate->tensorrt_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::AnnotateOutputDir:
            return access.annotate != nullptr ? BrowserFileDialogTarget{&access.annotate->output_dir}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ValidateCompiledPath:
            return access.validate != nullptr ? BrowserFileDialogTarget{&access.validate->compiled_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ValidateSourceRoot:
            return access.validate != nullptr ? BrowserFileDialogTarget{&access.validate->source_dir}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ValidateOnnx:
            return access.validate != nullptr ? BrowserFileDialogTarget{&access.validate->onnx_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ValidateTensorRt:
            return access.validate != nullptr ? BrowserFileDialogTarget{&access.validate->tensorrt_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ValidateSaveEngine:
            return access.validate != nullptr ? BrowserFileDialogTarget{&access.validate->save_engine_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ValidateReportJson:
            return access.validate != nullptr ? BrowserFileDialogTarget{&access.validate->report_json_path}
                                              : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ExportWeights:
            return access.export_state != nullptr ? BrowserFileDialogTarget{&access.export_state->weights_path}
                                                  : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ExportOnnx:
            return access.export_state != nullptr ? BrowserFileDialogTarget{&access.export_state->onnx_path}
                                                  : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::ExportOutputPath:
            return access.export_state != nullptr ? BrowserFileDialogTarget{&access.export_state->output_path}
                                                  : BrowserFileDialogTarget{};
        case BrowserFileDialogBinding::Unknown:
            break;
    }
    return {};
}

std::string browser_file_dialog_initial_path(const BrowserFileDialogBinding binding,
                                             const BrowserFileDialogStateAccess& access) {
    const BrowserFileDialogTarget target = browser_file_dialog_target(binding, access);
    return target.path != nullptr ? *target.path : std::string{};
}

void apply_browser_file_dialog_selection(const BrowserFileDialogBinding binding, std::string picked_path,
                                         const BrowserFileDialogStateAccess& access) {
    BrowserFileDialogTarget target = browser_file_dialog_target(binding, access);
    if (target.path == nullptr) {
        return;
    }
    if (target.source != nullptr && target.source_kind.has_value()) {
        target.source->kind = *target.source_kind;
    }
    *target.path = std::move(picked_path);
}

mmltk::gui::AnnotationMaskCleanupOp annotation_mask_cleanup_op_from_browser_name(const std::string_view name) {
    using CleanupOp = mmltk::gui::AnnotationMaskCleanupOp;
    constexpr std::array<BrowserNamedValue<CleanupOp>, 6> kCleanupOpNames{{
        {"largest_component", CleanupOp::LargestComponent},
        {"fill_holes", CleanupOp::FillHoles},
        {"dilate", CleanupOp::Dilate},
        {"erode", CleanupOp::Erode},
        {"open", CleanupOp::Open},
        {"close", CleanupOp::Close},
    }};
    if (const std::optional<CleanupOp> cleanup_op = find_browser_named_value(name, kCleanupOpNames);
        cleanup_op.has_value()) {
        return *cleanup_op;
    }
    throw std::runtime_error("unsupported browser annotate mask cleanup op: " + std::string(name));
}

BrowserFileDialogBinding file_dialog_binding_from_id(const std::string_view dialog_id) noexcept {
    const auto found =
        std::lower_bound(kBrowserNativeFileDialogsById.begin(), kBrowserNativeFileDialogsById.end(), dialog_id,
                         [](const BrowserNativeFileDialogContractSpec& spec, const std::string_view needle) {
                             return spec.id < needle;
                         });
    return found != kBrowserNativeFileDialogsById.end() && found->id == dialog_id ? found->binding
                                                                                  : BrowserFileDialogBinding::Unknown;
}

StateSnapshot make_state_snapshot(const GuiStateSnapshotInputs& inputs) {
    StateSnapshot snapshot;
    snapshot.state_revision = inputs.state_revision;
    snapshot.job = inputs.job;

    if (inputs.gui != nullptr) {
        snapshot.active_workflow = workflow_from_view(inputs.gui->current_view);
        snapshot.workflow_state = nlohmann::json{
            {"selected_preset", inputs.gui->selected_preset},
        };
        const nlohmann::json workflows = mmltk::gui::snapshot_workflows(inputs.gui->workflows);
        if (!workflows.empty()) {
            snapshot.workflow_state["workflows"] = workflows;
        }
        if (inputs.gui->ui_settings != nullptr) {
            snapshot.settings_state = *inputs.gui->ui_settings;
        }
        if (!inputs.source.has_value()) {
            if (const std::optional<SourceMetadata> inferred_source = infer_source_metadata(*inputs.gui);
                inferred_source.has_value()) {
                snapshot.source = *inferred_source;
            }
        }
    }

    if (inputs.source.has_value()) {
        snapshot.source = *inputs.source;
    }
    if (inputs.annotation.has_value()) {
        snapshot.annotation = *inputs.annotation;
    }
    snapshot.workspace_surface = inputs.workspace_surface;
    return snapshot;
}

bool apply_annotate_workspace_click(const AnnotateWorkspaceClickIntent& intent,
                                    const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.click");
    return context.controller->handle_canvas_click(*context.document, *context.session, *context.frame,
                                                   *context.categories, intent.capture_x, intent.capture_y,
                                                   intent.double_click);
}

bool apply_annotate_workspace_box_drag(const AnnotateWorkspaceBoxDragIntent& intent,
                                       const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.box_drag");
    if (intent.phase == AnnotateWorkspacePhase::Cancel) {
        return cancel_annotate_workspace_interaction(context);
    }

    const auto ensure_no_conflicting_interaction = [&]() {
        if (context.session->handle_drag().has_value()) {
            throw std::runtime_error("browser annotate.workspace.box_drag requires no active handle drag");
        }
        if (context.session->paint_stroke().active) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag requires "
                "no active brush stroke");
        }
        if (context.document->transaction_active()) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag requires "
                "no active annotation transaction");
        }
    };

    const auto update_drag_session = [&](mmltk::gui::PreviewRectDragSession& session) {
        if (intent.phase == AnnotateWorkspacePhase::End) {
            (void)mmltk::gui::update_preview_rect_drag(
                session, true, static_cast<float>(intent.capture_x), static_cast<float>(intent.capture_y),
                annotation_capture_identity_viewport(*context.frame),
                static_cast<int>(mmltk::gui::annotation_frame_capture_width(*context.frame)),
                static_cast<int>(mmltk::gui::annotation_frame_capture_height(*context.frame)), 1);
        }
        return mmltk::gui::update_preview_rect_drag(
            session, intent.phase != AnnotateWorkspacePhase::End, static_cast<float>(intent.capture_x),
            static_cast<float>(intent.capture_y), annotation_capture_identity_viewport(*context.frame),
            static_cast<int>(mmltk::gui::annotation_frame_capture_width(*context.frame)),
            static_cast<int>(mmltk::gui::annotation_frame_capture_height(*context.frame)), 1);
    };

    if (intent.phase == AnnotateWorkspacePhase::Begin) {
        ensure_no_conflicting_interaction();
        if (!intent.drag_kind.has_value()) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag begin "
                "requires payload.drag_kind");
        }

        if (*intent.drag_kind == AnnotateBoxDragKind::Create) {
            if (context.session->active_tool() != mmltk::gui::AnnotationToolKind::Box) {
                throw std::runtime_error("browser annotate.workspace.box_drag create requires the box tool");
            }
            if (context.session->create_drag_session().active || context.session->direct_drag_session().active) {
                throw std::runtime_error(
                    "browser annotate.workspace.box_drag begin "
                    "requires no active box drag");
            }

            mmltk::gui::start_preview_rect_drag(context.session->create_drag_session(),
                                                mmltk::gui::RectDragKind::Create, static_cast<float>(intent.capture_x),
                                                static_cast<float>(intent.capture_y),
                                                mmltk::gui::AnnotationBox{
                                                    intent.capture_x,
                                                    intent.capture_y,
                                                    intent.capture_x + 1,
                                                    intent.capture_y + 1,
                                                },
                                                kCreateCommitMinExtent);
            refresh_annotation_pointer_capture(*context.session);
            return false;
        }

        if (context.session->active_tool() != mmltk::gui::AnnotationToolKind::Direct) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag direct "
                "edits require the direct tool");
        }
        if (context.session->create_drag_session().active || context.session->direct_drag_session().active) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag begin "
                "requires no active box drag");
        }
        if (!intent.object_index.has_value()) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag begin requires "
                "payload.object_index for direct edits");
        }

        const auto object_index = static_cast<std::size_t>(*intent.object_index);
        const mmltk::gui::AnnotationObject* object = context.document->object(object_index);
        if (object == nullptr) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag begin "
                "requires a valid object_index");
        }
        const std::optional<mmltk::gui::AnnotationBox> box = mmltk::gui::annotation_object_display_box(*object);
        if (!box.has_value() || !mmltk::gui::annotation_box_has_area(*box)) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag begin "
                "requires an object with a visible box");
        }

        mmltk::gui::select_object(*context.session, *context.document, object_index);
        context.session->set_direct_drag_index(object_index);
        mmltk::gui::start_preview_rect_drag(
            context.session->direct_drag_session(), annotation_box_drag_kind_from_browser_kind(*intent.drag_kind),
            static_cast<float>(intent.capture_x), static_cast<float>(intent.capture_y), *box, 1);
        refresh_annotation_pointer_capture(*context.session);
        return false;
    }

    if (context.session->create_drag_session().active) {
        if (intent.drag_kind.has_value() && *intent.drag_kind != AnnotateBoxDragKind::Create) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag payload.drag_kind does not "
                "match the active drag");
        }
        if (intent.object_index.has_value()) {
            throw std::runtime_error(
                "browser annotate.workspace.box_drag create "
                "drag does not use payload.object_index");
        }
        const mmltk::gui::PreviewRectDragResult drag = update_drag_session(context.session->create_drag_session());
        bool changed = drag.changed;
        if (intent.phase == AnnotateWorkspacePhase::End && drag.commit) {
            const bool committed = context.controller->handle_box_commit(
                *context.document, *context.session, *context.frame, *context.categories,
                mmltk::gui::RectDragKind::Create, drag.box, default_box_create_category_index(context));
            if (committed) {
                context.controller->reset_active_drawing(*context.session);
            }
            changed = committed || changed;
        }
        refresh_annotation_pointer_capture(*context.session);
        return changed;
    }

    if (!context.session->direct_drag_session().active) {
        throw std::runtime_error("browser annotate.workspace.box_drag requires an active box drag");
    }

    const std::optional<AnnotateBoxDragKind> active_drag_kind =
        browser_box_drag_kind_from_session(context.session->direct_drag_session());
    if (!active_drag_kind.has_value() || *active_drag_kind == AnnotateBoxDragKind::Create) {
        throw std::runtime_error(
            "browser annotate.workspace.box_drag requires an "
            "active direct box drag");
    }
    if (intent.drag_kind.has_value() && *intent.drag_kind != *active_drag_kind) {
        throw std::runtime_error(
            "browser annotate.workspace.box_drag payload.drag_kind does not match "
            "the active drag");
    }
    if (intent.object_index.has_value() &&
        context.session->direct_drag_index() !=
            std::optional<std::size_t>{static_cast<std::size_t>(*intent.object_index)}) {
        throw std::runtime_error(
            "browser annotate.workspace.box_drag payload.object_index does not "
            "match the active drag");
    }

    const mmltk::gui::PreviewRectDragResult drag = update_drag_session(context.session->direct_drag_session());
    bool changed = drag.changed;
    if (intent.phase == AnnotateWorkspacePhase::End) {
        const auto& object_index = context.session->direct_drag_index();
        if (drag.commit && object_index.has_value()) {
            changed = context.controller->handle_box_commit(
                          *context.document, *context.session, *context.frame, *context.categories,
                          annotation_box_drag_kind_from_browser_kind(*active_drag_kind), drag.box, 0U) ||
                      changed;
        }
        context.session->set_direct_drag_index(std::nullopt);
    }
    refresh_annotation_pointer_capture(*context.session);
    return changed;
}

bool apply_annotate_workspace_handle_drag(const AnnotateWorkspaceHandleDragIntent& intent,
                                          const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.handle_drag");
    if (intent.phase == AnnotateWorkspacePhase::Cancel) {
        return cancel_annotate_workspace_interaction(context);
    }
    if (context.session->active_tool() != mmltk::gui::AnnotationToolKind::Direct) {
        throw std::runtime_error("browser annotate.workspace.handle_drag requires the direct tool");
    }

    const auto resolve_handle = [&]() -> mmltk::gui::AnnotationHandleId {
        if (intent.handle.has_value()) {
            return annotation_handle_id_from_browser_ref(*intent.handle);
        }
        const auto& active_drag = context.session->handle_drag();
        if (!active_drag.has_value() || !active_drag->active) {
            throw std::runtime_error(
                "browser annotate.workspace.handle_drag "
                "requires an active handle drag");
        }
        return active_drag->handle;
    };

    if (intent.phase == AnnotateWorkspacePhase::Begin) {
        if (!intent.handle.has_value()) {
            throw std::runtime_error(
                "browser annotate.workspace.handle_drag begin "
                "requires payload.handle");
        }
        if (context.session->handle_drag().has_value()) {
            throw std::runtime_error(
                "browser annotate.workspace.handle_drag begin "
                "requires no active handle drag");
        }
        if (context.session->paint_stroke().active) {
            throw std::runtime_error(
                "browser annotate.workspace.handle_drag begin "
                "requires no active brush stroke");
        }
        if (context.document->transaction_active()) {
            throw std::runtime_error(
                "browser annotate.workspace.handle_drag begin "
                "requires no active annotation transaction");
        }

        const mmltk::gui::AnnotationHandleId handle = annotation_handle_id_from_browser_ref(*intent.handle);
        if (!annotation_handle_exists(*context.document, handle)) {
            throw std::runtime_error(
                "browser annotate.workspace.handle_drag begin "
                "requires a valid editable handle");
        }

        mmltk::gui::select_object(*context.session, *context.document, handle.object_index);
        context.session->begin_handle_drag(handle);
        context.document->begin_transaction();
        const bool changed =
            context.controller->handle_handle_drag(*context.document, *context.session, *context.frame,
                                                   *context.categories, handle, intent.capture_x, intent.capture_y);
        refresh_annotation_pointer_capture(*context.session);
        return changed;
    }

    const auto& active_drag = context.session->handle_drag();
    if (!active_drag.has_value() || !active_drag->active) {
        throw std::runtime_error(
            "browser annotate.workspace.handle_drag requires "
            "an active handle drag");
    }

    const mmltk::gui::AnnotationHandleId handle = resolve_handle();
    if (!(handle == active_drag->handle)) {
        throw std::runtime_error(
            "browser annotate.workspace.handle_drag payload.handle does not match "
            "the active drag handle");
    }

    const bool changed =
        context.controller->handle_handle_drag(*context.document, *context.session, *context.frame, *context.categories,
                                               handle, intent.capture_x, intent.capture_y);
    if (intent.phase == AnnotateWorkspacePhase::End) {
        if (context.document->transaction_active()) {
            (void)context.document->commit_transaction();
        }
        context.session->clear_handle_drag();
    }
    refresh_annotation_pointer_capture(*context.session);
    return changed;
}

bool apply_annotate_workspace_brush(const AnnotateWorkspaceBrushIntent& intent,
                                    const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.brush");
    if (intent.phase == AnnotateWorkspacePhase::Cancel) {
        return cancel_annotate_workspace_interaction(context);
    }
    if (intent.radius <= 0) {
        throw std::runtime_error("browser annotate.workspace.brush requires a positive radius");
    }
    if (!mmltk::gui::annotation_tool_kind_uses_brush(context.session->active_tool())) {
        throw std::runtime_error(
            "browser annotate.workspace.brush requires the "
            "mask paint or erase tool");
    }
    if (selected_mask_editable_object(*context.document, *context.session) == nullptr) {
        throw std::runtime_error(
            "browser annotate.workspace.brush requires a "
            "selected mask-editable object");
    }

    const bool erase = context.session->active_tool() == mmltk::gui::AnnotationToolKind::MaskErase;
    const auto apply_segment = [&](const int x0, const int y0, const int x1, const int y1) {
        bool changed = false;
        mmltk::gui::rasterize_line_samples(x0, y0, x1, y1, [&](const int sample_x, const int sample_y) {
            changed = context.controller->handle_brush_sample(*context.document, *context.session, *context.frame,
                                                              *context.categories, sample_x, sample_y, intent.radius) ||
                      changed;
        });
        return changed;
    };

    if (intent.phase == AnnotateWorkspacePhase::Begin) {
        if (context.session->paint_stroke().active) {
            throw std::runtime_error(
                "browser annotate.workspace.brush begin "
                "requires no active brush stroke");
        }
        if (context.session->handle_drag().has_value()) {
            throw std::runtime_error(
                "browser annotate.workspace.brush begin "
                "requires no active handle drag");
        }
        if (context.document->transaction_active()) {
            throw std::runtime_error(
                "browser annotate.workspace.brush begin "
                "requires no active annotation transaction");
        }

        context.session->begin_paint_stroke(intent.capture_x, intent.capture_y);
        context.document->begin_transaction();
        const bool changed = apply_segment(intent.capture_x, intent.capture_y, intent.capture_x, intent.capture_y);
        set_annotation_brush_preview(*context.session, intent.capture_x, intent.capture_y, intent.radius, erase);
        refresh_annotation_pointer_capture(*context.session);
        return changed;
    }

    if (!context.session->paint_stroke().active) {
        throw std::runtime_error("browser annotate.workspace.brush requires an active brush stroke");
    }

    const mmltk::gui::AnnotationPaintStroke stroke = context.session->paint_stroke();
    const bool changed =
        apply_segment(stroke.last_capture_x, stroke.last_capture_y, intent.capture_x, intent.capture_y);
    if (intent.phase == AnnotateWorkspacePhase::End) {
        if (context.document->transaction_active()) {
            (void)context.document->commit_transaction();
        }
        context.session->clear_paint_stroke();
        context.session->set_brush_preview(mmltk::gui::AnnotationBrushPreview{});
    } else {
        context.session->update_paint_stroke_position(intent.capture_x, intent.capture_y);
        set_annotation_brush_preview(*context.session, intent.capture_x, intent.capture_y, intent.radius, erase);
    }
    refresh_annotation_pointer_capture(*context.session);
    return changed;
}

bool apply_annotate_workspace_fill(const AnnotateWorkspaceFillIntent& intent,
                                   const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.fill");
    if (context.session->active_tool() != mmltk::gui::AnnotationToolKind::MaskFill) {
        throw std::runtime_error("browser annotate.workspace.fill requires the mask fill tool");
    }
    if (selected_mask_editable_object(*context.document, *context.session) == nullptr) {
        throw std::runtime_error(
            "browser annotate.workspace.fill requires a "
            "selected mask-editable object");
    }

    return context.controller->handle_fill(*context.document, *context.session, *context.frame, *context.categories,
                                           intent.capture_x, intent.capture_y);
}

bool apply_annotate_workspace_color_sample(const AnnotateWorkspaceColorSampleIntent& intent,
                                           const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_document_context(context, "annotate.workspace.color_sample");
    const std::optional<std::size_t> selected_index = selected_object_index(*context.document, *context.session);
    const mmltk::gui::AnnotationObject* selected_object =
        selected_mask_editable_object(*context.document, *context.session);
    if (!selected_index.has_value() || selected_object == nullptr) {
        throw std::runtime_error(
            "browser annotate.workspace.color_sample requires "
            "a selected mask-editable object");
    }

    mmltk::gui::AnnotationColorRange sup = selected_object->sup;
    mmltk::gui::AnnotationColorRange nosup = selected_object->nosup;
    mmltk::gui::AnnotationColorRange* armed_range = sup.sampling ? &sup : nosup.sampling ? &nosup : nullptr;
    if (armed_range == nullptr) {
        throw std::runtime_error(
            "browser annotate.workspace.color_sample requires "
            "an armed sup or nosup range");
    }

    if (context.frame->pixels_bgr.empty()) {
        if (!context.ensure_frame_pixels) {
            throw std::runtime_error("browser annotate.workspace.color_sample requires frame pixels");
        }
        std::string ensure_error;
        if (!context.ensure_frame_pixels(&ensure_error)) {
            throw std::runtime_error(ensure_error.empty() ? "browser annotate.workspace.color_sample "
                                                            "could not load frame pixels"
                                                          : ensure_error);
        }
    }

    const int frame_x = intent.capture_x - static_cast<int>(context.frame->view_x);
    const int frame_y = intent.capture_y - static_cast<int>(context.frame->view_y);
    const mmltk::gui::AnnotationHsv sampled = mmltk::gui::sample_annotation_hsv(*context.frame, frame_x, frame_y);
    mmltk::gui::recenter_annotation_range(*armed_range, sampled);
    sup.sampling = false;
    nosup.sampling = false;
    return mmltk::gui::AnnotationEditor::update_selected_object_color_ranges(*context.document, *context.session, sup,
                                                                             nosup);
}

bool apply_annotate_workspace_pointer(const AnnotateWorkspacePointerIntent& intent,
                                      const BrowserAnnotateWorkspaceContext& context) {
    require_annotate_workspace_context(context, "annotate.workspace.pointer");

    if (intent.drag_kind.has_value()) {
        AnnotateWorkspaceBoxDragIntent drag;
        drag.phase = intent.phase;
        drag.drag_kind = intent.drag_kind;
        drag.object_index = intent.object_index;
        drag.capture_x = intent.capture_x;
        drag.capture_y = intent.capture_y;
        return apply_annotate_workspace_box_drag(drag, context);
    }

    if (intent.handle.has_value()) {
        AnnotateWorkspaceHandleDragIntent drag;
        drag.phase = intent.phase;
        drag.handle = intent.handle;
        drag.capture_x = intent.capture_x;
        drag.capture_y = intent.capture_y;
        return apply_annotate_workspace_handle_drag(drag, context);
    }

    if (intent.tool == "mask.color_sample") {
        if (intent.phase == AnnotateWorkspacePhase::End) {
            return apply_annotate_workspace_color_sample(
                AnnotateWorkspaceColorSampleIntent{intent.capture_x, intent.capture_y}, context);
        }
        return intent.phase == AnnotateWorkspacePhase::Cancel ? cancel_annotate_workspace_interaction(context) : false;
    }

    const mmltk::gui::AnnotationToolKind tool = annotation_tool_kind_from_name_impl(intent.tool);
    if (mmltk::gui::annotation_tool_kind_uses_brush(tool)) {
        AnnotateWorkspaceBrushIntent brush;
        brush.phase = intent.phase;
        brush.capture_x = intent.capture_x;
        brush.capture_y = intent.capture_y;
        brush.radius = intent.brush_radius;
        return apply_annotate_workspace_brush(brush, context);
    }
    if (intent.phase == AnnotateWorkspacePhase::Cancel) {
        return cancel_annotate_workspace_interaction(context);
    }
    if (intent.phase != AnnotateWorkspacePhase::End) {
        return false;
    }
    if (tool == mmltk::gui::AnnotationToolKind::MaskFill) {
        return apply_annotate_workspace_fill(
            AnnotateWorkspaceFillIntent{intent.capture_x, intent.capture_y}, context);
    }
    if (tool == mmltk::gui::AnnotationToolKind::Point || tool == mmltk::gui::AnnotationToolKind::Spline ||
        tool == mmltk::gui::AnnotationToolKind::Skeleton) {
        return apply_annotate_workspace_click(
            AnnotateWorkspaceClickIntent{intent.capture_x, intent.capture_y, false}, context);
    }
    return false;
}

void apply_settings_update(const SettingsUpdateIntent& intent, mmltk::gui::GuiSettingsSnapshot& snapshot) {
    nlohmann::json current = mmltk::gui::snapshot_gui_settings(snapshot);
    current.merge_patch(normalize_settings_patch(intent.patch));
    current["schema_version"] = mmltk::gui::kGuiSettingsSchemaVersion;
    apply_gui_settings(current, snapshot);
}

void apply_crop_commit(const CropCommitIntent& intent, mmltk::gui::SourceSelectionState& source) noexcept {
    if (!intent.has_crop) {
        source.crop_x = 0;
        source.crop_y = 0;
        source.crop_width = 0;
        source.crop_height = 0;
        return;
    }

    source.crop_x = saturating_int(intent.crop.x);
    source.crop_y = saturating_int(intent.crop.y);
    source.crop_width = saturating_int(intent.crop.width);
    source.crop_height = saturating_int(intent.crop.height);
}

struct BrowserHostIntentApplyHandlers {
    const BrowserHostIntentContext& context;
    const BrowserAnnotateWorkspaceContext& annotate_workspace_context;

    void bind_annotate_view() const noexcept {
        if (context.current_view != nullptr) {
            *context.current_view = mmltk::gui::View::Annotate;
        }
    }

    void require_annotate_workflow(const RoutedIntent& routed, const std::string_view intent_name) const {
        if (routed.workflow != Workflow::Annotate) {
            throw std::runtime_error("browser " + std::string(intent_name) +
                                     " is only bound for the annotate workflow");
        }
    }

    void operator()(const RoutedIntent&, const SettingsUpdateIntent& settings) const {
        if (context.gui == nullptr) {
            throw std::runtime_error("browser settings.update requires a bound gui settings snapshot");
        }
        apply_settings_update(settings, *context.gui);
        if (context.current_view != nullptr) {
            *context.current_view = context.gui->current_view;
        }
        if (context.selected_preset != nullptr) {
            *context.selected_preset = context.gui->selected_preset;
        }
    }

    void operator()(const RoutedIntent& routed, const CropCommitIntent& crop) const {
        mmltk::gui::SourceSelectionState* source = source_selection_for_workflow(routed.workflow, context);
        if (source == nullptr) {
            throw std::runtime_error(
                "browser crop.commit requires a bound source "
                "selection for the target workflow");
        }
        apply_crop_commit(crop, *source);
    }

    void operator()(const RoutedIntent& routed, const ToolSelectIntent& tool) const {
        require_annotate_workflow(routed, "tool.select");
        if (context.annotation_controller == nullptr || context.annotation_document == nullptr ||
            context.annotation_session == nullptr) {
            throw std::runtime_error("browser tool.select requires bound annotation controller state");
        }
        const mmltk::gui::AnnotationToolKind next_tool = annotation_tool_kind_from_name(tool.tool);
        if (!context.annotation_controller->set_active_tool(next_tool, *context.annotation_document,
                                                            *context.annotation_session)) {
            throw std::runtime_error("browser tool.select could not activate the requested tool");
        }
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspaceClickIntent& click) const {
        require_annotate_workflow(routed, "annotate.workspace.click");
        (void)apply_annotate_workspace_click(click, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspaceBoxDragIntent& box_drag) const {
        require_annotate_workflow(routed, "annotate.workspace.box_drag");
        (void)apply_annotate_workspace_box_drag(box_drag, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspaceHandleDragIntent& handle_drag) const {
        require_annotate_workflow(routed, "annotate.workspace.handle_drag");
        (void)apply_annotate_workspace_handle_drag(handle_drag, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspaceBrushIntent& brush) const {
        require_annotate_workflow(routed, "annotate.workspace.brush");
        (void)apply_annotate_workspace_brush(brush, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspacePointerIntent& pointer) const {
        require_annotate_workflow(routed, "annotate.workspace.pointer");
        (void)apply_annotate_workspace_pointer(pointer, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspaceFillIntent& fill) const {
        require_annotate_workflow(routed, "annotate.workspace.fill");
        (void)apply_annotate_workspace_fill(fill, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent& routed, const AnnotateWorkspaceColorSampleIntent& color_sample) const {
        require_annotate_workflow(routed, "annotate.workspace.color_sample");
        (void)apply_annotate_workspace_color_sample(color_sample, annotate_workspace_context);
        bind_annotate_view();
    }

    void operator()(const RoutedIntent&, const WorkspaceRendererEventIntent&) const {}

    template <typename Payload>
    void operator()(const RoutedIntent& routed, const Payload&) const {
        throw std::runtime_error("browser host app intent not yet bound: " + std::string(routed_intent_name(routed)));
    }
};

void apply_routed_intent(const RoutedIntent& intent, const BrowserHostIntentContext& context) {
    const BrowserAnnotateWorkspaceContext annotate_workspace_context{
        context.annotation_document, context.annotation_session,    context.annotation_controller,
        context.annotation_frame,    context.annotation_categories, context.ensure_annotation_frame_pixels,
    };

    BrowserIntentDispatchFactory<BrowserIntentPayloadTypes, BrowserHostIntentApplyHandlers>::dispatch(
        intent, BrowserHostIntentApplyHandlers{context, annotate_workspace_context});
}

FrameSlotDescriptor frame_slot_from_output_bundle(std::string slot_name, const FrameTransportKind transport,
                                                  const FramePixelFormat pixel_format,
                                                  const mmltk::live::OutputBundle& bundle) {
    return make_frame_slot_descriptor(std::move(slot_name), transport, pixel_format, bundle.slot_index, bundle.frame_id,
                                      bundle.dims, bundle.region, bundle.ready_ns, bundle.short_frame);
}

FrameSlotDescriptor frame_slot_from_workspace_output_bundle(std::string slot_name, const FrameTransportKind transport,
                                                            const FramePixelFormat pixel_format,
                                                            const mmltk::live::WorkspaceOutputBundle& bundle) {
    return make_frame_slot_descriptor(std::move(slot_name), transport, pixel_format, bundle.slot_index, bundle.frame_id,
                                      bundle.dims, bundle.region, bundle.ready_ns, bundle.short_frame);
}

FrameSlotDescriptor frame_slot_from_bgr_frame(std::string slot_name, const std::uint32_t slot_index,
                                              const mmltk::live::LiveFrameId& frame_id,
                                              const CaptureRegion capture_region, const std::uint32_t width,
                                              const std::uint32_t height,
                                              const std::uint64_t published_row_stride_bytes,
                                              const std::uint64_t ready_ns, const bool short_frame) {
    struct Dimensions {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t pitch_bytes = 0;
    };

    return make_frame_slot_descriptor(std::move(slot_name), FrameTransportKind::CudaDeviceBuffer,
                                      FramePixelFormat::Rgba8, slot_index, frame_id,
                                      Dimensions{width, height, published_row_stride_bytes},
                                      mmltk::live::LiveCaptureRegion{
                                          capture_region.x,
                                          capture_region.y,
                                          capture_region.width,
                                          capture_region.height,
                                      },
                                      ready_ns, short_frame);
}

}  // namespace mmltk::browser
