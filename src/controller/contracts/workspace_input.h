#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "src/controller/contracts/visual_source.h"
#include "src/controller/contracts/annotation.h"

namespace mmltk::controller {

enum class WorkspaceMouseKind : std::uint8_t { Motion, Press, Release, Wheel, Enter, Leave, Cancel };
enum class WorkspaceWheelUnit : std::uint8_t { Lines, Pixels };
enum class WorkspaceMouseButton : std::uint8_t { Left, Right, Middle, Back, Forward, Other };

struct WorkspacePoint final {
    float x = 0.0F;
    float y = 0.0F;
    [[nodiscard]] bool valid() const noexcept { return std::isfinite(x) && std::isfinite(y); }
    auto operator<=>(const WorkspacePoint&) const = default;
};

// Source and document epochs identify logical ownership, never a rendered image.
// Coordinates are optional so empty workspaces use exactly the same ingress.
struct WorkspaceMouse final {
    PresentationSourceKind source = PresentationSourceKind::None;
    std::uint64_t peer_epoch = 0U;
    std::uint64_t document_epoch = 0U;
    WorkspaceMouseKind kind = WorkspaceMouseKind::Motion;
    std::optional<WorkspacePoint> point{};
    WorkspaceMouseButton button = WorkspaceMouseButton::Left;
    std::uint16_t other_button = 0U;
    std::uint8_t click_count = 0U;
    std::uint8_t modifiers = 0U;
    WorkspaceWheelUnit wheel_unit = WorkspaceWheelUnit::Lines;
    WorkspacePoint wheel{};
    [[= mmltk::frameworks::reflection::Minimum<std::uint16_t>{contracts::kMinAnnotationBrushRadius}]]
    [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{contracts::kMaxAnnotationBrushRadius}]]
    std::uint16_t brush_radius = contracts::kDefaultAnnotationBrushRadius;
    [[nodiscard]] bool valid() const noexcept {
        namespace reflection = mmltk::frameworks::reflection;
        return presentation_source_session(source) != 0U && peer_epoch != 0U &&
               reflection::enum_contains(kind) && reflection::enum_contains(button) &&
               reflection::enum_contains(wheel_unit) && (!point || point->valid()) && wheel.valid() &&
               modifiers <= 15U && brush_radius >= contracts::kMinAnnotationBrushRadius &&
               brush_radius <= contracts::kMaxAnnotationBrushRadius;
    }
};

MMLTK_REFLECT_ENUM(WorkspaceMouseKind)
MMLTK_REFLECT_ENUM(WorkspaceWheelUnit)
MMLTK_REFLECT_ENUM(WorkspaceMouseButton)
MMLTK_REFLECT_FIELDS(WorkspacePoint)
MMLTK_REFLECT_FIELDS(WorkspaceMouse)

}  // namespace mmltk::controller
