#include "gui/annotation/tools/default_tools.h"
#include "gui/annotation/tools/skeleton_tool.h"
#include "gui/annotation/tools/spline_tool.h"
#include "gui/annotation/tools/tool_helpers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace mmltk::gui {

namespace {

template <AnnotationToolKind Kind>
class ClearingAnnotationTool : public AnnotationTool {
   public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept final {
        return Kind;
    }

    void reset_active_drawing(AnnotationSession& session) final {
        session.clear_transient_state();
    }
};

[[nodiscard]] AnnotationToolMutation make_tool_update_mutation(const bool changed, const bool reset_canvas = false) {
    return AnnotationToolMutation{changed, false, std::nullopt, reset_canvas};
}

class SelectionTool final : public ClearingAnnotationTool<AnnotationToolKind::Select> {};

class DirectTool final : public ClearingAnnotationTool<AnnotationToolKind::Direct> {
   public:
    [[nodiscard]] AnnotationToolMutation handle_box_commit(const AnnotationToolContext& context,
                                                           const AnnotationToolBoxCommitEvent& event) override {
        const auto& object_index = context.session.direct_drag_index();
        if (!object_index.has_value() || *object_index >= context.document.size()) {
            return {};
        }

        bool changed = false;
        if (event.drag_kind == RectDragKind::Move) {
            const AnnotationObject* object = context.document.object(*object_index);
            if (object == nullptr) {
                return {};
            }
            const std::optional<AnnotationBox> current_box = annotation_object_display_box(*object);
            if (!current_box.has_value()) {
                return {};
            }
            changed = context.document.apply(AnnotationTranslateObjectCommand{
                *object_index,
                event.capture_box.x1 - current_box->x1,
                event.capture_box.y1 - current_box->y1,
                context.capture_width,
                context.capture_height,
            });
        } else {
            changed = context.document.apply(AnnotationResizeObjectCommand{
                *object_index,
                event.capture_box,
                context.capture_width,
                context.capture_height,
            });
        }

        return make_tool_update_mutation(changed);
    }

    bool handle_handle_drag(const AnnotationToolContext& context, const AnnotationToolHandleDragEvent& event) override {
        return context.document.apply(AnnotationSetHandlePositionCommand{
            event.handle,
            AnnotationPoint{
                static_cast<float>(event.capture_x),
                static_cast<float>(event.capture_y),
            },
            context.capture_width,
            context.capture_height,
        });
    }
};

class BoxTool final : public ClearingAnnotationTool<AnnotationToolKind::Box> {
   public:
    [[nodiscard]] AnnotationToolMutation handle_box_commit(const AnnotationToolContext& context,
                                                           const AnnotationToolBoxCommitEvent& event) override {
        if (event.drag_kind != RectDragKind::Create) {
            return {};
        }

        const auto& selected_index = context.session.selected_object_index();
        if (selected_index.has_value()) {
            const AnnotationObject* selected_object = context.document.object(*selected_index);
            if (selected_object != nullptr && annotation_object_supports_mask_editing(*selected_object)) {
                const std::optional<AnnotationBox> current_box = annotation_object_display_box(*selected_object);
                if (!current_box.has_value() || !annotation_box_has_area(*current_box)) {
                    const bool changed = context.document.apply(AnnotationSetObjectBoxCommand{
                        *selected_index,
                        event.capture_box,
                        context.capture_width,
                        context.capture_height,
                    });
                    return make_tool_update_mutation(changed, changed);
                }
            }
        }

        const bool changed = context.document.apply(AnnotationInsertObjectCommand{
            make_box_annotation_object(context.document.size(), event.default_category_index, event.capture_box),
            std::nullopt,
        });
        return AnnotationToolMutation{
            changed,
            changed,
            changed ? std::optional<std::size_t>{context.document.size() - 1U} : std::nullopt,
            changed,
        };
    }

    [[nodiscard]] std::optional<AnnotationObject> make_object(
        const AnnotationToolCreateObjectRequest& request) const override {
        return make_box_annotation_object(request.object_count, request.category_index, {});
    }
};

template <AnnotationToolKind Kind, bool Erase>
class MaskBrushTool final : public ClearingAnnotationTool<Kind> {
   public:
    bool handle_brush_sample(const AnnotationToolContext& context, const AnnotationToolBrushEvent& event) override {
        const auto& selected_index = context.session.selected_object_index();
        if (!selected_index.has_value()) {
            return false;
        }
        return context.document.apply(AnnotationPaintMaskCommand{
            *selected_index,
            event.capture_x,
            event.capture_y,
            std::max(1, event.radius),
            Erase,
            context.capture_width,
            context.capture_height,
        });
    }
};

using MaskPaintTool = MaskBrushTool<AnnotationToolKind::MaskPaint, false>;
using MaskEraseTool = MaskBrushTool<AnnotationToolKind::MaskErase, true>;

class MaskFillTool final : public ClearingAnnotationTool<AnnotationToolKind::MaskFill> {
   public:
    bool handle_fill(const AnnotationToolContext& context, const AnnotationToolFillEvent& event) override {
        const auto& selected_index = context.session.selected_object_index();
        if (!selected_index.has_value()) {
            return false;
        }
        return context.document.apply(AnnotationFillMaskCommand{
            *selected_index,
            event.capture_x,
            event.capture_y,
            context.capture_width,
            context.capture_height,
        });
    }
};

class PointTool final : public ClearingAnnotationTool<AnnotationToolKind::Point> {
   public:
    [[nodiscard]] AnnotationToolMutation handle_canvas_click(const AnnotationToolContext& context,
                                                             const AnnotationToolCanvasClickEvent& event) override {
        const AnnotationPoint point = tool_detail::capture_point_from_event(event);
        const auto& selected_index = context.session.selected_object_index();
        if (selected_index.has_value()) {
            const AnnotationObject* selected_object = context.document.object(*selected_index);
            if (selected_object != nullptr && std::holds_alternative<AnnotationPointShape>(selected_object->shape)) {
                const bool changed = context.document.apply(AnnotationSetPointPositionCommand{
                    *selected_index,
                    point,
                    context.capture_width,
                    context.capture_height,
                });
                return make_tool_update_mutation(changed);
            }
        }

        if (!context.document.apply(AnnotationInsertObjectCommand{
                make_point_annotation_object(context.document.size(), event.default_category_index, point),
                std::nullopt,
            })) {
            return {};
        }
        return AnnotationToolMutation{
            true,
            true,
            context.document.size() - 1U,
            false,
        };
    }

    [[nodiscard]] std::optional<AnnotationObject> make_object(
        const AnnotationToolCreateObjectRequest& request) const override {
        return make_point_annotation_object(request.object_count, request.category_index,
                                            request.point.value_or(AnnotationPoint{}));
    }
};

class AnnotationToolCreator {
   public:
    virtual ~AnnotationToolCreator() = default;

    [[nodiscard]] virtual AnnotationToolKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<AnnotationTool> create_tool() const = 0;
};

template <AnnotationToolKind Kind, typename Tool>
class StaticAnnotationToolCreator final : public AnnotationToolCreator {
   public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override {
        return Kind;
    }
    [[nodiscard]] std::unique_ptr<AnnotationTool> create_tool() const override {
        return std::make_unique<Tool>();
    }
};

template <AnnotationToolKind Kind, std::unique_ptr<AnnotationTool> (*Factory)()>
class FunctionAnnotationToolCreator final : public AnnotationToolCreator {
   public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override {
        return Kind;
    }
    [[nodiscard]] std::unique_ptr<AnnotationTool> create_tool() const override {
        return Factory();
    }
};

const std::array<const AnnotationToolCreator*, annotation_tool_kind_count()>& default_tool_creators() {
    static const StaticAnnotationToolCreator<AnnotationToolKind::Select, SelectionTool> selection_creator;
    static const StaticAnnotationToolCreator<AnnotationToolKind::Direct, DirectTool> direct_creator;
    static const StaticAnnotationToolCreator<AnnotationToolKind::Box, BoxTool> box_creator;
    static const StaticAnnotationToolCreator<AnnotationToolKind::MaskPaint, MaskPaintTool> mask_paint_creator;
    static const StaticAnnotationToolCreator<AnnotationToolKind::MaskErase, MaskEraseTool> mask_erase_creator;
    static const StaticAnnotationToolCreator<AnnotationToolKind::MaskFill, MaskFillTool> mask_fill_creator;
    static const FunctionAnnotationToolCreator<AnnotationToolKind::Spline, make_spline_annotation_tool> spline_creator;
    static const StaticAnnotationToolCreator<AnnotationToolKind::Point, PointTool> point_creator;
    static const FunctionAnnotationToolCreator<AnnotationToolKind::Skeleton, make_skeleton_annotation_tool>
        skeleton_creator;
    static const std::array<const AnnotationToolCreator*, annotation_tool_kind_count()> creators{{
        &selection_creator,
        &direct_creator,
        &box_creator,
        &mask_paint_creator,
        &mask_erase_creator,
        &mask_fill_creator,
        &spline_creator,
        &point_creator,
        &skeleton_creator,
    }};
    return creators;
}

const AnnotationToolCreator* find_default_tool_creator(const AnnotationToolKind kind) noexcept {
    const std::size_t slot = static_cast<std::size_t>(kind);
    if (slot >= default_tool_creators().size()) {
        return nullptr;
    }
    const AnnotationToolCreator* creator = default_tool_creators()[slot];
    return creator != nullptr && creator->kind() == kind ? creator : nullptr;
}

}  // namespace

std::unique_ptr<AnnotationTool> make_annotation_tool(const AnnotationToolKind kind) {
    if (const AnnotationToolCreator* creator = find_default_tool_creator(kind); creator != nullptr) {
        return creator->create_tool();
    }
    throw std::runtime_error("annotation tool kind is out of range");
}

void register_default_annotation_tools(AnnotationToolManager& manager) {
    for (const AnnotationToolCreator* creator : default_tool_creators()) {
        manager.register_tool(creator->create_tool());
    }
}

}  // namespace mmltk::gui
