#pragma once

#include "gui/annotation_core.h"
#include "gui/annotation/document/document.h"
#include "gui/annotation/session.h"

#include <array>
#include <memory>
#include <optional>

namespace mmltk::gui {

struct AnnotationToolContext {
    AnnotationDocument& document;
    AnnotationSession& session;
    const AnnotationFrame& frame;
    const AnnotationCategories& categories;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationToolCanvasClickEvent {
    int capture_x = 0;
    int capture_y = 0;
    bool double_click = false;
    std::size_t default_category_index = 0;
};

struct AnnotationToolBoxCommitEvent {
    RectDragKind drag_kind = RectDragKind::None;
    AnnotationBox capture_box{};
    std::size_t default_category_index = 0;
};

struct AnnotationToolBrushEvent {
    int capture_x = 0;
    int capture_y = 0;
    int radius = 1;
};

struct AnnotationToolHandleDragEvent {
    AnnotationHandleId handle{};
    int capture_x = 0;
    int capture_y = 0;
};

struct AnnotationToolFillEvent {
    int capture_x = 0;
    int capture_y = 0;
};

enum class AnnotationToolActionKind : std::uint8_t {
    Confirm = 0,
    Cancel = 1,
    DeleteActiveElement = 2,
    CycleHandleMode = 3,
    ReopenSpline = 4,
    SkipJoint = 5,
    HideJoint = 6,
    ReactivateJoint = 7,
    ReseedJoint = 8,
};

struct AnnotationToolActionEvent {
    AnnotationToolActionKind action = AnnotationToolActionKind::Confirm;
};

struct AnnotationToolCreateObjectRequest {
    std::size_t object_count = 0;
    std::size_t category_index = 0;
    std::optional<AnnotationPoint> point;
    const AnnotationCategory* category = nullptr;
};

struct AnnotationToolMutation {
    bool changed = false;
    bool selection_changed = false;
    std::optional<std::size_t> selected_object_index;
    bool clear_transient_state = false;
};

class AnnotationTool {
   public:
    virtual ~AnnotationTool() = default;

    [[nodiscard]] virtual AnnotationToolKind kind() const noexcept = 0;
    virtual void reset_active_drawing(AnnotationSession& session) = 0;
    [[nodiscard]] virtual AnnotationToolMutation handle_canvas_click(const AnnotationToolContext&,
                                                                     const AnnotationToolCanvasClickEvent&);
    [[nodiscard]] virtual AnnotationToolMutation handle_box_commit(const AnnotationToolContext&,
                                                                   const AnnotationToolBoxCommitEvent&);
    virtual bool handle_handle_drag(const AnnotationToolContext&, const AnnotationToolHandleDragEvent&);
    virtual bool handle_brush_sample(const AnnotationToolContext&, const AnnotationToolBrushEvent&);
    virtual bool handle_fill(const AnnotationToolContext&, const AnnotationToolFillEvent&);
    virtual AnnotationToolMutation handle_action(const AnnotationToolContext&, const AnnotationToolActionEvent&);
    [[nodiscard]] virtual std::optional<AnnotationObject> make_object(const AnnotationToolCreateObjectRequest&) const;
    virtual void on_object_created(AnnotationDocument& document, AnnotationSession& session,
                                   std::size_t object_index) const;
};

class AnnotationToolManager {
   public:
    void register_tool(std::unique_ptr<AnnotationTool> tool);
    [[nodiscard]] AnnotationTool* find_tool(AnnotationToolKind kind) noexcept;
    [[nodiscard]] const AnnotationTool* find_tool(AnnotationToolKind kind) const noexcept;
    bool set_active_tool(AnnotationToolKind kind, AnnotationDocument& document, AnnotationSession& session);

   private:
    std::array<std::unique_ptr<AnnotationTool>, annotation_tool_kind_count()> tools_{};
};

}  
