#pragma once

#include "gui/annotation/tools/tool_manager.h"

namespace mmltk::gui {

class AnnotationController {
   public:
    AnnotationController();

    [[nodiscard]] AnnotationToolManager& tools() noexcept {
        return tools_;
    }
    [[nodiscard]] const AnnotationToolManager& tools() const noexcept {
        return tools_;
    }

    bool set_active_tool(AnnotationToolKind kind, AnnotationDocument& document, AnnotationSession& session);
    void reset_active_drawing(AnnotationSession& session);
    bool handle_canvas_click(AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame& frame,
                             const AnnotationCategories& categories, int capture_x, int capture_y, bool double_click);
    bool handle_box_commit(AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame& frame,
                           const AnnotationCategories& categories, RectDragKind drag_kind,
                           const AnnotationBox& capture_box, std::size_t default_category_index);
    bool handle_handle_drag(AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame& frame,
                            const AnnotationCategories& categories, const AnnotationHandleId& handle, int capture_x,
                            int capture_y);
    bool handle_brush_sample(AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame& frame,
                             const AnnotationCategories& categories, int capture_x, int capture_y, int radius);
    bool handle_fill(AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame& frame,
                     const AnnotationCategories& categories, int capture_x, int capture_y);
    bool handle_tool_action(AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame& frame,
                            const AnnotationCategories& categories, AnnotationToolActionKind action);
    bool handle_tool_action_for_kind(AnnotationToolKind kind, AnnotationDocument& document, AnnotationSession& session,
                                     const AnnotationFrame& frame, const AnnotationCategories& categories,
                                     AnnotationToolActionKind action);
    std::optional<std::size_t> create_object(AnnotationToolKind kind, AnnotationDocument& document,
                                             AnnotationSession& session, const AnnotationFrame& frame,
                                             const AnnotationCategories& categories, std::size_t category_index);

   private:
    static AnnotationToolContext make_tool_context(AnnotationDocument& document, AnnotationSession& session,
                                                   const AnnotationFrame& frame,
                                                   const AnnotationCategories& categories);
    static bool apply_tool_mutation(const AnnotationToolMutation& mutation, AnnotationDocument& document,
                                    AnnotationSession& session);

    AnnotationToolManager tools_;
};

}  
