#pragma once

#include "gui/annotation/tools/tool_manager.h"

namespace mmltk::gui::tool_detail {

[[nodiscard]] AnnotationEditTransactionKind grouped_edit_kind_for_tool(AnnotationToolKind tool) noexcept;
AnnotationPoint capture_point_from_event(const AnnotationToolCanvasClickEvent& event);
std::optional<std::size_t> clamp_active_index(std::size_t size, std::optional<std::size_t> index);

bool begin_grouped_tool_edit(const AnnotationToolContext& context, AnnotationToolKind tool,
                             std::optional<std::size_t> object_index,
                             std::optional<std::size_t> restore_selection_index);
bool begin_grouped_tool_edit(AnnotationDocument& document, AnnotationSession& session, AnnotationToolKind tool,
                             std::optional<std::size_t> object_index,
                             std::optional<std::size_t> restore_selection_index);
void bind_grouped_tool_edit_object(AnnotationSession& session, std::size_t object_index);
bool commit_grouped_tool_edit(const AnnotationToolContext& context, AnnotationToolKind tool);
bool commit_grouped_tool_edit(AnnotationDocument& document, AnnotationSession& session, AnnotationToolKind tool);
bool cancel_grouped_tool_edit(const AnnotationToolContext& context, AnnotationToolKind tool);
bool cancel_grouped_tool_edit(AnnotationDocument& document, AnnotationSession& session, AnnotationToolKind tool,
                              std::optional<std::size_t> selection_after_cancel = std::nullopt);
bool cancel_active_grouped_tool_edit(AnnotationDocument& document, AnnotationSession& session,
                                     std::optional<std::size_t> selection_after_cancel = std::nullopt);

}  
