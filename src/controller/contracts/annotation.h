#pragma once
#include "src/controller/contracts/annotation_limits.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/workflows.h"
#include "src/controller/contracts/workspace.h"
#include "src/backend/data/catalog/class_catalog.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kAnnotationNameCapacity = 96U;
inline constexpr std::size_t kAnnotationObjectCapacity = 4096U;
inline constexpr std::size_t kAnnotationCategoryCapacity = mmltk::backend::data::catalog::kClassCatalogCapacity;
inline constexpr std::size_t kAnnotationMaskRunCapacity = 32768U;
inline constexpr std::size_t kAnnotationGeometryCapacity = 8U;
inline constexpr std::size_t kAnnotationUiStateByteBudget = 8U * 1024U * 1024U;
inline constexpr mmltk::frameworks::reflection::FixedText kAnnotationTextPolicy{
    .capacity = kAnnotationNameCapacity,
    .characters = mmltk::frameworks::reflection::FixedTextCharacterPolicy::PrintableAscii,
};
enum class AnnotationTool : std::uint8_t { Select, Box, MaskPaint, MaskErase, MaskFill, Spline, Point, Skeleton, ColorSample };
enum class AnnotationShape : std::uint8_t { Box, Mask, Spline, Point, Skeleton };
enum class AnnotationPointerPhase : std::uint8_t { Begin, Update, End, Cancel };
enum class AnnotationSplineHandleMode : std::uint8_t { Corner, Smooth, Mirrored };
enum class AnnotationHandleRole : std::uint8_t { Point, SplineKnot, SplineInHandle, SplineOutHandle, SkeletonNode, BoxCorner };
enum class AnnotationMaskCleanup : std::uint8_t { LargestComponent, FillHoles, Dilate, Erode, Open, Close };
enum class AnnotationSaveStatus : std::uint8_t { Idle, Saved, Failed, Uncertain };
enum class AnnotationSidebarCommand : std::uint8_t {
    Assist,
    Delete,
    Duplicate,
    Undo,
    Redo,
    RedrawBox,
    SplineInsertKnot,
    SplineClose,
    SplineReopen,
    SplineDeleteKnot,
    SkeletonSkip,
    SkeletonHide,
    SkeletonShow,
    SkeletonReseed
};
enum class AnnotationSetupAction : std::uint8_t { StartLive, StopLive, ReloadFrame, PreviousFrame, NextFrame };
MMLTK_REFLECT_ENUM(AnnotationTool)
MMLTK_REFLECT_ENUM(AnnotationShape)
MMLTK_REFLECT_ENUM(AnnotationPointerPhase)
MMLTK_REFLECT_ENUM(AnnotationSplineHandleMode)
MMLTK_REFLECT_ENUM(AnnotationHandleRole)
MMLTK_REFLECT_ENUM(AnnotationMaskCleanup)
MMLTK_REFLECT_ENUM(AnnotationSaveStatus)
MMLTK_REFLECT_ENUM(AnnotationSidebarCommand)
MMLTK_REFLECT_ENUM(AnnotationSetupAction)
struct[[= kAnnotationTextPolicy]] AnnotationText final {
    std::array<char, kAnnotationTextPolicy.capacity> bytes{};
    std::uint8_t size = 0U;
    [[nodiscard]] static AnnotationText From(const std::string_view value) noexcept {
        AnnotationText result{};
        if (!kAnnotationTextPolicy.accepts(value)) { return result; }
        result.size = static_cast<std::uint8_t>(value.size());
        std::copy_n(value.data(), value.size(), result.bytes.data());
        return result;
    }
    [[nodiscard]] bool valid() const noexcept {
        return size <= bytes.size() && kAnnotationTextPolicy.accepts({bytes.data(), size}) &&
               std::ranges::all_of(bytes.begin() + size, bytes.end(), [](const char item) { return item == '\0'; });
    }
    [[nodiscard]] std::string_view view() const noexcept { return valid() ? std::string_view{bytes.data(), size} : std::string_view{}; }
    auto operator<=>(const AnnotationText&) const = default;
};
struct AnnotationPoint final {
    float x = 0.0F;
    float y = 0.0F;
    [[nodiscard]] bool finite() const noexcept { return std::isfinite(x) && std::isfinite(y); }
    auto operator<=>(const AnnotationPoint&) const = default;
};
struct AnnotationBox final {
    AnnotationPoint first{};
    AnnotationPoint second{};
    [[nodiscard]] bool valid() const noexcept { return first.finite() && second.finite() && first.x <= second.x && first.y <= second.y; }
    auto operator<=>(const AnnotationBox&) const = default;
};
struct AnnotationColor final {
    float hue = 0.0F;
    float saturation = 0.0F;
    float value = 0.0F;
    [[nodiscard]] bool valid() const noexcept {
        return std::isfinite(hue) && std::isfinite(saturation) && std::isfinite(value) && hue >= 0.0F && hue <= 360.0F && saturation >= 0.0F &&
               saturation <= 1.0F && value >= 0.0F && value <= 1.0F;
    }
    auto operator<=>(const AnnotationColor&) const = default;
};
struct AnnotationColorRange final {
    AnnotationColor center{};
    AnnotationColor minus{};
    AnnotationColor plus{};
    bool sampling = false;
    [[nodiscard]] bool valid() const noexcept { return center.valid() && minus.valid() && plus.valid(); }
    auto operator<=>(const AnnotationColorRange&) const = default;
};
struct AnnotationSplineHandle final {
    AnnotationPoint point{};
    bool enabled = false;
    [[nodiscard]] bool valid() const noexcept { return point.finite(); }
    auto operator<=>(const AnnotationSplineHandle&) const = default;
};
struct AnnotationSplineKnot final {
    AnnotationPoint point{};
    AnnotationSplineHandle in{};
    AnnotationSplineHandle out{};
    AnnotationSplineHandleMode mode = AnnotationSplineHandleMode::Corner;
    [[nodiscard]] bool valid() const noexcept { return point.finite() && in.valid() && out.valid() && mmltk::frameworks::reflection::enum_contains(mode); }
    auto operator<=>(const AnnotationSplineKnot&) const = default;
};
struct AnnotationSkeletonNode final {
    AnnotationText key{};
    AnnotationPoint point{};
    bool visible = true;
    [[nodiscard]] bool valid() const noexcept { return key.valid() && point.finite(); }
    auto operator<=>(const AnnotationSkeletonNode&) const = default;
};
struct AnnotationEdge final {
    std::uint16_t source = 0U;
    std::uint16_t target = 0U;
    auto operator<=>(const AnnotationEdge&) const = default;
};
struct AnnotationMaskRun final {
    std::uint16_t row = 0U;
    std::uint16_t first = 0U;
    std::uint16_t last = 0U;
    [[nodiscard]] bool valid() const noexcept { return first <= last; }
    auto operator<=>(const AnnotationMaskRun&) const = default;
};
struct AnnotationMask final {
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationMaskRunCapacity}]] std::vector<AnnotationMaskRun> runs{};
    std::uint16_t cleanup_radius = 0U;
    AnnotationMaskCleanup cleanup = AnnotationMaskCleanup::LargestComponent;
    bool present = false;
    [[nodiscard]] bool valid() const noexcept {
        return runs.size() <= kAnnotationMaskRunCapacity && mmltk::frameworks::reflection::enum_contains(cleanup) &&
               std::ranges::all_of(runs, [](const auto& run) { return run.valid(); });
    }
    auto operator<=>(const AnnotationMask&) const = default;
};
struct AnnotationObject final {
    AnnotationText name{};
    AnnotationShape shape = AnnotationShape::Box;
    AnnotationBox box{};
    AnnotationPoint point{};
    AnnotationMask mask{};
    AnnotationColorRange sup{};
    // CLEANUP-IGNORE: Object geometry and scene catalogs are distinct canonical bounded fields, not parallel mappings.
    AnnotationColorRange nosup{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationGeometryCapacity}]] std::vector<AnnotationPoint> mask_points{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationGeometryCapacity}]] std::vector<AnnotationSplineKnot> spline_knots{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationGeometryCapacity}]] std::vector<AnnotationSkeletonNode> skeleton_nodes{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationGeometryCapacity}]] std::vector<AnnotationEdge> skeleton_edges{};
    std::uint16_t category = 0U;
    bool spline_closed = false;
    bool enabled = true;
    [[nodiscard]] bool valid() const noexcept {
        if (mask_points.size() > kAnnotationGeometryCapacity || spline_knots.size() > kAnnotationGeometryCapacity ||
            skeleton_nodes.size() > kAnnotationGeometryCapacity || skeleton_edges.size() > kAnnotationGeometryCapacity)
            return false;
        if (!name.valid() || !mmltk::frameworks::reflection::enum_contains(shape) || !point.finite() || !mask.valid() || !sup.valid() || !nosup.valid()) {
            return false;
        }
        if (shape == AnnotationShape::Box && !box.valid()) return false;
        if (!std::ranges::all_of(mask_points, [](const auto& item) { return item.finite(); }) ||
            !std::ranges::all_of(spline_knots, [](const auto& item) { return item.valid(); }) ||
            !std::ranges::all_of(skeleton_nodes, [](const auto& item) { return item.valid(); })) {
            return false;
        }
        return std::ranges::all_of(skeleton_edges, [this](const auto& edge) {
            return edge.source < skeleton_nodes.size() && edge.target < skeleton_nodes.size() && edge.source != edge.target;
        });
    }
    auto operator<=>(const AnnotationObject&) const = default;
};
// Runtime custody facts are deliberately separate from persisted object content.
struct AnnotationTargetIdentity final {
    std::uint64_t object = 0U;
    std::uint64_t element = 0U;
    auto operator<=>(const AnnotationTargetIdentity&) const = default;
};
struct AnnotationObjectIdentity final {
    std::uint64_t object = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationGeometryCapacity}]] std::vector<std::uint64_t> elements{};
    auto operator<=>(const AnnotationObjectIdentity&) const = default;
};
struct AnnotationPointerTarget final {
    std::optional<std::uint16_t> object{};
    std::optional<std::uint16_t> element{};
    std::optional<AnnotationHandleRole> role{};
    [[nodiscard]] bool valid() const noexcept {
        return (!role || mmltk::frameworks::reflection::enum_contains(*role)) && element.has_value() == role.has_value() && (!element || object.has_value());
    }
    auto operator<=>(const AnnotationPointerTarget&) const = default;
};
// These facts are the editor portion of the public state. Collections and the
// System-derived facts remain outside AnnotationSceneContent so scene authors
// cannot mint lifecycle, capability, selection, or pointer state.
struct AnnotationEditorFacts final {
    std::optional<std::uint16_t> selected_object{};
    std::optional<std::uint16_t> selected_category{};
    std::optional<std::uint16_t> selected_spline_segment{};
    std::optional<std::uint16_t> selected_skeleton_joint{};
    AnnotationTool tool = AnnotationTool::Select;
    bool hold_save = false;
    bool assist_available = false;
    bool assist_running = false;
    auto operator<=>(const AnnotationEditorFacts&) const = default;
};
// This is the one canonical scene value. It is embedded once in the public UI
// state, accepted for complete scene installation, and encoded unchanged by
// persistence. It deliberately has no history or operation identity.
struct AnnotationSceneContent final {
    WorkspaceResource document{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationCategoryCapacity}]] std::vector<mmltk::backend::data::catalog::ClassName> categories{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationCategoryCapacity}]] std::vector<AnnotationColor> palette{};
    [[= mmltk::frameworks::reflection::MaxItems{kAnnotationObjectCapacity}]] std::vector<AnnotationObject> objects{};
    std::uint16_t frame_width = 0U;
    std::uint16_t frame_height = 0U;
    std::uint32_t frame_index = 0U;
    bool frame_ready = false;
    [[nodiscard]] bool valid() const noexcept {
        if (!document.valid() || categories.size() > kAnnotationCategoryCapacity || objects.size() > kAnnotationObjectCapacity ||
            palette.size() > kAnnotationCategoryCapacity || (!palette.empty() && palette.size() != categories.size()) ||
            !std::ranges::all_of(palette, [](const auto& color) { return color.valid(); }))
            return false;
        std::size_t mask_runs = 0U;
        for (const auto& object : objects) {
            if (object.mask.runs.size() > kAnnotationMaskRunCapacity - mask_runs) return false;
            mask_runs += object.mask.runs.size();
        }
        if (frame_ready && (frame_width == 0U || frame_height == 0U)) return false;
        const auto point_in_frame = [this](const AnnotationPoint point) {
            return !frame_ready || (point.x >= 0.0F && point.y >= 0.0F && point.x <= frame_width && point.y <= frame_height);
        };
        if (!std::ranges::all_of(categories, [](const auto& item) { return item.valid(); }) ||
            !std::ranges::all_of(objects, [this, &point_in_frame](const auto& item) {
                return item.valid() && item.category < categories.size() && point_in_frame(item.point) && point_in_frame(item.box.first) &&
                       point_in_frame(item.box.second) &&
                       std::ranges::all_of(
                           item.mask.runs, [this](const auto& run) { return !frame_ready || (run.row < frame_height && run.last < frame_width); }) &&
                       std::ranges::all_of(item.mask_points, point_in_frame) &&
                       std::ranges::all_of(
                           item.spline_knots,
                           [&point_in_frame](const auto& knot) {
                               return point_in_frame(knot.point) && point_in_frame(knot.in.point) && point_in_frame(knot.out.point);
                           }) &&
                       std::ranges::all_of(item.skeleton_nodes, [&point_in_frame](const auto& node) { return point_in_frame(node.point); });
            })) {
            return false;
        }
        return true;
    }
    auto operator<=>(const AnnotationSceneContent&) const = default;
};
// The sole public and persisted Annotation state. ApplicationUiState, the
// reflected schema, UI projection, and persistence all use
// this exact type. Private editor work remains inside AnnotationSystem.
struct AnnotationToolCapability final {
    AnnotationTool tool = AnnotationTool::Select;
    bool available = false;
    auto operator<=>(const AnnotationToolCapability&) const = default;
};
struct AnnotationUiState final {
    AnnotationSceneContent scene{};
    AnnotationEditorFacts editor{};
    [[= mmltk::frameworks::reflection::MaxItems{mmltk::frameworks::reflection::enum_entries<AnnotationTool>().size()}]] std::vector<AnnotationToolCapability>
        tool_capabilities{};
    bool can_undo = false;
    bool can_redo = false;
    bool source_navigation_available = false;
    AnnotationSaveStatus save_status = AnnotationSaveStatus::Idle;
    std::uint64_t interaction_revision = 0U;
    std::uint64_t document_revision = 0U;
    std::uint64_t saved_revision = 0U;
    std::uint64_t scene_revision = 0U;
    [[nodiscard]] bool empty() const noexcept {
        return tool_capabilities.empty() && !can_undo && !can_redo && !source_navigation_available && !scene.document.valid() && scene.categories.empty() &&
               scene.palette.empty() && scene.objects.empty() && editor == AnnotationEditorFacts{} && save_status == AnnotationSaveStatus::Idle &&
               interaction_revision == 0U && document_revision == 0U && saved_revision == 0U && scene_revision == 0U;
    }
    [[nodiscard]] bool valid() const noexcept {
        if (!scene.document.valid()) return empty();
        if (!scene.valid() || !mmltk::frameworks::reflection::enum_contains(editor.tool)) return false;
        if (editor.selected_object && *editor.selected_object >= scene.objects.size()) return false;
        if (editor.selected_category && *editor.selected_category >= scene.categories.size()) return false;
        return mmltk::frameworks::reflection::enum_contains(save_status) && document_revision != 0U && saved_revision <= document_revision &&
               scene_revision != 0U && interaction_revision != 0U && scene.document.revision == document_revision;
    }
    // CLEANUP-IGNORE: AnnotationUiState closes its canonical domain declaration before the reflected inventory.
    auto operator<=>(const AnnotationUiState&) const = default;
};
// CLEANUP-IGNORE: These registrations materialize distinct canonical Annotation types; structural projection is already shared.
MMLTK_REFLECT_FIELDS(AnnotationText)
MMLTK_REFLECT_FIELDS(AnnotationPoint)
MMLTK_REFLECT_FIELDS(AnnotationBox)
MMLTK_REFLECT_FIELDS(AnnotationColor)
MMLTK_REFLECT_FIELDS(AnnotationColorRange)
MMLTK_REFLECT_FIELDS(AnnotationSplineHandle)
MMLTK_REFLECT_FIELDS(AnnotationSplineKnot)
MMLTK_REFLECT_FIELDS(AnnotationSkeletonNode)
MMLTK_REFLECT_FIELDS(AnnotationEdge)
MMLTK_REFLECT_FIELDS(AnnotationMaskRun)
MMLTK_REFLECT_FIELDS(AnnotationMask)
MMLTK_REFLECT_FIELDS(AnnotationObject)
MMLTK_REFLECT_FIELDS(AnnotationPointerTarget)
MMLTK_REFLECT_FIELDS(AnnotationTargetIdentity)
MMLTK_REFLECT_FIELDS(AnnotationObjectIdentity)
MMLTK_REFLECT_FIELDS(AnnotationEditorFacts)
MMLTK_REFLECT_FIELDS(AnnotationSceneContent)
MMLTK_REFLECT_FIELDS(AnnotationToolCapability)
MMLTK_REFLECT_FIELDS(AnnotationUiState)
// The annotation vocabulary owns its persistent representation. Keeping the
// reflected specialization behind this ordinary function gives every caller
// one schema authority and one template instantiation site.
[[nodiscard]] bool encode_annotation_persistence(const AnnotationUiState& state, std::vector<std::byte>& destination) noexcept;
}  // namespace mmltk::controller::contracts
