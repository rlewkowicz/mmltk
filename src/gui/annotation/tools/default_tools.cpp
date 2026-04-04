#include "gui/annotation/tools/default_tools.h"
#include "gui/annotation/tools/skeleton_tool.h"
#include "gui/annotation/tools/spline_tool.h"
#include "gui/annotation/tools/tool_helpers.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace mmltk::gui {

namespace {

class SelectionTool final : public AnnotationTool {
public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override { return AnnotationToolKind::Select; }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }
};

class DirectTool final : public AnnotationTool {
public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override { return AnnotationToolKind::Direct; }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }

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

        return AnnotationToolMutation{
            changed,
            false,
            std::nullopt,
            false,
        };
    }

    bool handle_handle_drag(const AnnotationToolContext& context,
                            const AnnotationToolHandleDragEvent& event) override {
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

class BoxTool final : public AnnotationTool {
public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override { return AnnotationToolKind::Box; }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }

    [[nodiscard]] AnnotationToolMutation handle_box_commit(const AnnotationToolContext& context,
                                                           const AnnotationToolBoxCommitEvent& event) override {
        if (event.drag_kind != RectDragKind::Create) {
            return {};
        }

        const auto& selected_index = context.session.selected_object_index();
        if (selected_index.has_value()) {
            const AnnotationObject* selected_object = context.document.object(*selected_index);
            if (selected_object != nullptr &&
                annotation_object_supports_mask_editing(*selected_object)) {
                const std::optional<AnnotationBox> current_box =
                    annotation_object_display_box(*selected_object);
                if (!current_box.has_value() || !annotation_box_has_area(*current_box)) {
                    const bool changed = context.document.apply(AnnotationSetObjectBoxCommand{
                        *selected_index,
                        event.capture_box,
                        context.capture_width,
                        context.capture_height,
                    });
                    return AnnotationToolMutation{
                        changed,
                        false,
                        std::nullopt,
                        changed,
                    };
                }
            }
        }

        AnnotationObject object;
        object.object_id = next_annotation_object_id(context.document.size());
        object.category_index = event.default_category_index;
        object.shape = AnnotationBoxShape{event.capture_box};
        const bool changed =
            context.document.apply(AnnotationInsertObjectCommand{std::move(object), std::nullopt});
        return AnnotationToolMutation{
            changed,
            changed,
            changed ? std::optional<std::size_t>{context.document.size() - 1U} : std::nullopt,
            changed,
        };
    }

    [[nodiscard]] std::optional<AnnotationObject> make_object(
        const AnnotationToolCreateObjectRequest& request) const override {
        AnnotationObject object;
        object.object_id = next_annotation_object_id(request.object_count);
        object.category_index = request.category_index;
        object.shape = AnnotationBoxShape{};
        return object;
    }
};

class MaskBrushTool final : public AnnotationTool {
public:
    explicit MaskBrushTool(const AnnotationToolKind tool_kind, const bool erase) noexcept
        : tool_kind_(tool_kind), erase_(erase) {}

    [[nodiscard]] AnnotationToolKind kind() const noexcept override { return tool_kind_; }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }

    bool handle_brush_sample(const AnnotationToolContext& context,
                             const AnnotationToolBrushEvent& event) override {
        const auto& selected_index = context.session.selected_object_index();
        if (!selected_index.has_value()) {
            return false;
        }
        return context.document.apply(AnnotationPaintMaskCommand{
            *selected_index,
            event.capture_x,
            event.capture_y,
            std::max(1, event.radius),
            erase_,
            context.capture_width,
            context.capture_height,
        });
    }

private:
    AnnotationToolKind tool_kind_;
    bool erase_;
};

class MaskFillTool final : public AnnotationTool {
public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override { return AnnotationToolKind::MaskFill; }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }

    bool handle_fill(const AnnotationToolContext& context,
                     const AnnotationToolFillEvent& event) override {
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

class PointTool final : public AnnotationTool {
public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override { return AnnotationToolKind::Point; }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }

    [[nodiscard]] AnnotationToolMutation handle_canvas_click(const AnnotationToolContext& context,
                                                             const AnnotationToolCanvasClickEvent& event) override {
        const AnnotationPoint point = tool_detail::capture_point_from_event(event);
        const auto& selected_index = context.session.selected_object_index();
        if (selected_index.has_value()) {
            const AnnotationObject* selected_object = context.document.object(*selected_index);
            if (selected_object != nullptr &&
                std::holds_alternative<AnnotationPointShape>(selected_object->shape)) {
                const bool changed = context.document.apply(AnnotationSetPointPositionCommand{
                    *selected_index,
                    point,
                    context.capture_width,
                    context.capture_height,
                });
                return AnnotationToolMutation{
                    changed,
                    false,
                    std::nullopt,
                    false,
                };
            }
        }

        AnnotationObject object =
            make_point_annotation_object(context.document.size(), event.default_category_index, point);
        if (!context.document.apply(AnnotationInsertObjectCommand{std::move(object), std::nullopt})) {
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
        return make_point_annotation_object(request.object_count,
                                            request.category_index,
                                            request.point.value_or(AnnotationPoint{}));
    }
};

} // namespace

std::unique_ptr<AnnotationTool> make_annotation_tool(const AnnotationToolKind kind) {
    switch (kind) {
    case AnnotationToolKind::Select:
        return std::make_unique<SelectionTool>();
    case AnnotationToolKind::Direct:
        return std::make_unique<DirectTool>();
    case AnnotationToolKind::Box:
        return std::make_unique<BoxTool>();
    case AnnotationToolKind::MaskPaint:
        return std::make_unique<MaskBrushTool>(AnnotationToolKind::MaskPaint, false);
    case AnnotationToolKind::MaskErase:
        return std::make_unique<MaskBrushTool>(AnnotationToolKind::MaskErase, true);
    case AnnotationToolKind::MaskFill:
        return std::make_unique<MaskFillTool>();
    case AnnotationToolKind::Spline:
        return make_spline_annotation_tool();
    case AnnotationToolKind::Point:
        return std::make_unique<PointTool>();
    case AnnotationToolKind::Skeleton:
        return make_skeleton_annotation_tool();
    case AnnotationToolKind::Count:
        break;
    }
    throw std::runtime_error("annotation tool kind is out of range");
}

void register_default_annotation_tools(AnnotationToolManager& manager) {
    for (const AnnotationToolKind kind : {
             AnnotationToolKind::Select,
             AnnotationToolKind::Direct,
             AnnotationToolKind::Box,
             AnnotationToolKind::MaskPaint,
             AnnotationToolKind::MaskErase,
             AnnotationToolKind::MaskFill,
             AnnotationToolKind::Spline,
             AnnotationToolKind::Point,
             AnnotationToolKind::Skeleton,
         }) {
        manager.register_tool(make_annotation_tool(kind));
    }
}

} // namespace mmltk::gui
