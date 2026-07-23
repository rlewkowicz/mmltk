#include "gui/annotation/tools/skeleton_tool.h"

#include "gui/annotation/document/edit.h"
#include "gui/annotation/tools/skeleton_tool_helpers.h"

#include <memory>

namespace mmltk::gui {

namespace {

class SkeletonTool final : public AnnotationTool {
   public:
    [[nodiscard]] AnnotationToolKind kind() const noexcept override {
        return AnnotationToolKind::Skeleton;
    }

    void reset_active_drawing(AnnotationSession& session) override {
        session.clear_transient_state();
    }

    [[nodiscard]] AnnotationToolMutation handle_canvas_click(const AnnotationToolContext& context,
                                                             const AnnotationToolCanvasClickEvent& event) override {
        return tool_detail::handle_skeleton_canvas_click(context, event);
    }

    [[nodiscard]] AnnotationToolMutation handle_action(const AnnotationToolContext& context,
                                                       const AnnotationToolActionEvent& event) override {
        return tool_detail::handle_skeleton_action(context, event);
    }

    [[nodiscard]] std::optional<AnnotationObject> make_object(
        const AnnotationToolCreateObjectRequest& request) const override {
        return make_skeleton_annotation_object(request.object_count, request.category_index, request.category);
    }

    void on_object_created(AnnotationDocument& document, AnnotationSession& session,
                           const std::size_t object_index) const override {
        tool_detail::finalize_created_skeleton_object(document, session, object_index);
    }
};

}  

std::unique_ptr<AnnotationTool> make_skeleton_annotation_tool() {
    return std::make_unique<SkeletonTool>();
}

}  
