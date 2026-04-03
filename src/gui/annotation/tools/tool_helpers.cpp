#include "gui/annotation/tools/tool_helpers.h"

#include "gui/annotation/editor_history.h"

#include <algorithm>

namespace mmltk::gui::tool_detail {

AnnotationEditTransactionKind grouped_edit_kind_for_tool(const AnnotationToolKind tool) noexcept {
    switch (tool) {
    case AnnotationToolKind::Spline:
        return AnnotationEditTransactionKind::SplineConstruction;
    case AnnotationToolKind::Skeleton:
        return AnnotationEditTransactionKind::SkeletonConstruction;
    case AnnotationToolKind::Select:
    case AnnotationToolKind::Direct:
    case AnnotationToolKind::Box:
    case AnnotationToolKind::MaskPaint:
    case AnnotationToolKind::MaskErase:
    case AnnotationToolKind::MaskFill:
    case AnnotationToolKind::Point:
    case AnnotationToolKind::Count:
        break;
    }
    return AnnotationEditTransactionKind::None;
}

AnnotationPoint capture_point_from_event(const AnnotationToolCanvasClickEvent& event) {
    return AnnotationPoint{
        static_cast<float>(event.capture_x),
        static_cast<float>(event.capture_y),
    };
}

std::optional<std::size_t> clamp_active_index(const std::size_t size,
                                              const std::optional<std::size_t> index) {
    if (!index.has_value() || size == 0U) {
        return std::nullopt;
    }
    return std::min(*index, size - 1U);
}

bool begin_grouped_tool_edit(const AnnotationToolContext& context,
                             const AnnotationToolKind tool,
                             const std::optional<std::size_t> object_index,
                             const std::optional<std::size_t> restore_selection_index) {
    const AnnotationEditTransactionKind kind = grouped_edit_kind_for_tool(tool);
    if (kind == AnnotationEditTransactionKind::None) {
        return true;
    }
    return begin_grouped_edit(context.document,
                              context.session,
                              kind,
                              object_index,
                              restore_selection_index);
}

bool begin_grouped_tool_edit(AnnotationDocument& document,
                             AnnotationSession& session,
                             const AnnotationToolKind tool,
                             const std::optional<std::size_t> object_index,
                             const std::optional<std::size_t> restore_selection_index) {
    const AnnotationEditTransactionKind kind = grouped_edit_kind_for_tool(tool);
    if (kind == AnnotationEditTransactionKind::None) {
        return true;
    }
    return begin_grouped_edit(document,
                              session,
                              kind,
                              object_index,
                              restore_selection_index);
}

void bind_grouped_tool_edit_object(AnnotationSession& session, const std::size_t object_index) {
    const AnnotationGroupedEditTransactionState current_state =
        session.grouped_edit_transaction();
    if (current_state.kind == AnnotationEditTransactionKind::None) {
        return;
    }
    session.set_grouped_edit_transaction(AnnotationGroupedEditTransactionState{
        current_state.kind,
        object_index,
        current_state.restore_selection_index,
    });
}

bool commit_grouped_tool_edit(const AnnotationToolContext& context,
                              const AnnotationToolKind tool) {
    return commit_grouped_edit(context.document,
                               context.session,
                               grouped_edit_kind_for_tool(tool));
}

bool commit_grouped_tool_edit(AnnotationDocument& document,
                              AnnotationSession& session,
                              const AnnotationToolKind tool) {
    return commit_grouped_edit(document, session, grouped_edit_kind_for_tool(tool));
}

bool cancel_grouped_tool_edit(const AnnotationToolContext& context,
                              const AnnotationToolKind tool) {
    return cancel_grouped_edit(context.document,
                               context.session,
                               grouped_edit_kind_for_tool(tool));
}

bool cancel_grouped_tool_edit(AnnotationDocument& document,
                              AnnotationSession& session,
                              const AnnotationToolKind tool,
                              std::optional<std::size_t> selection_after_cancel) {
    return cancel_grouped_edit(document,
                               session,
                               grouped_edit_kind_for_tool(tool),
                               selection_after_cancel);
}

bool cancel_active_grouped_tool_edit(AnnotationDocument& document,
                                     AnnotationSession& session,
                                     std::optional<std::size_t> selection_after_cancel) {
    return cancel_active_grouped_edit(document, session, selection_after_cancel);
}

} // namespace mmltk::gui::tool_detail
