#include "gui/annotation/tools/tool_manager.h"

#include "gui/annotation/tools/tool_helpers.h"

namespace mmltk::gui {

namespace {

constexpr std::size_t tool_slot(const AnnotationToolKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

template <typename Tools>
auto* find_tool_in(Tools& tools, const AnnotationToolKind kind) noexcept {
    const std::size_t slot = tool_slot(kind);
    return slot < tools.size() ? tools[slot].get() : nullptr;
}

}  

AnnotationToolMutation AnnotationTool::handle_canvas_click(const AnnotationToolContext&,
                                                           const AnnotationToolCanvasClickEvent&) {
    return {};
}

AnnotationToolMutation AnnotationTool::handle_box_commit(const AnnotationToolContext&,
                                                         const AnnotationToolBoxCommitEvent&) {
    return {};
}

bool AnnotationTool::handle_handle_drag(const AnnotationToolContext&, const AnnotationToolHandleDragEvent&) {
    return false;
}

bool AnnotationTool::handle_brush_sample(const AnnotationToolContext&, const AnnotationToolBrushEvent&) {
    return false;
}

bool AnnotationTool::handle_fill(const AnnotationToolContext&, const AnnotationToolFillEvent&) {
    return false;
}

AnnotationToolMutation AnnotationTool::handle_action(const AnnotationToolContext&, const AnnotationToolActionEvent&) {
    return {};
}

std::optional<AnnotationObject> AnnotationTool::make_object(const AnnotationToolCreateObjectRequest&) const {
    return std::nullopt;
}

void AnnotationTool::on_object_created(AnnotationDocument&, AnnotationSession&, const std::size_t) const {}

void AnnotationToolManager::register_tool(std::unique_ptr<AnnotationTool> tool) {
    if (tool == nullptr) {
        return;
    }
    const std::size_t slot = tool_slot(tool->kind());
    if (slot >= tools_.size()) {
        return;
    }
    tools_[slot] = std::move(tool);
}

AnnotationTool* AnnotationToolManager::find_tool(const AnnotationToolKind kind) noexcept {
    return find_tool_in(tools_, kind);
}

const AnnotationTool* AnnotationToolManager::find_tool(const AnnotationToolKind kind) const noexcept {
    return find_tool_in(tools_, kind);
}

bool AnnotationToolManager::set_active_tool(const AnnotationToolKind kind, AnnotationDocument& document,
                                            AnnotationSession& session) {
    AnnotationTool* next = find_tool(kind);
    if (next == nullptr) {
        return false;
    }
    if (session.active_tool() == kind) {
        return true;
    }
    (void)tool_detail::cancel_active_grouped_tool_edit(document, session);
    if (AnnotationTool* current = find_tool(session.active_tool()); current != nullptr) {
        current->reset_active_drawing(session);
    } else {
        next->reset_active_drawing(session);
    }
    session.set_active_tool(kind);
    return true;
}

}  
